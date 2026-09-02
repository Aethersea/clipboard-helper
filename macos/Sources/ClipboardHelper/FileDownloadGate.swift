import Foundation

// MARK: - FileDownloadGate
//
// The wait/coordination half of the legacy `.fileURL` paste path, pulled
// out of `PasteboardManager.ensureFilesDownloaded` so the timeout, cancel
// and supersede rules can be unit-tested without an NSPasteboard or an
// NSApplication in the process.  `PasteboardManager` keeps everything that
// touches AppKit (the data-provider callback, the run-loop pump, the
// DATA_REQUEST send) and hands the gate closures for those pieces.
//
//   * The FIRST caller for an announcement ("primary") sends DATA_REQUEST
//     and waits for the render semaphore to be signalled by PROVIDE_DATA,
//     polling the cancel flag and the "still current" predicate between
//     ticks and yielding to `idle` so the main run loop keeps pumping.
//   * Any LATER caller while the primary is still waiting ("secondary")
//     does not send another request; it spins until the primary publishes
//     a path list, gives up, is cancelled, or the announcement is
//     superseded.
//   * Once a path list is published, every subsequent caller gets it back
//     immediately — the OS asks once per NSPasteboardItem, so a 3-file
//     paste is one primary + two secondaries + cached reads.
//
// Timeout semantics: `timeout` is a STALL timeout, not a wall-clock cap.
// The parent reports FILE_TRANSFER_PROGRESS frames while the download is
// moving; each one is fed in through `noteProgress()` and pushes the
// deadline out.  A large file that is still transferring is therefore
// never abandoned mid-flight; only `timeout` seconds of silence gives up.
//
// Failure semantics: a primary that times out or fails does not poison
// the announcement.  It hands its outcome to the secondaries that were
// waiting on it and re-arms the gate so the next caller becomes a fresh
// primary (a new DATA_REQUEST).  Only `cancel()` sticks until `reset()`.
//
// Attempt identity: every primary is one "attempt", numbered by
// `generation`.  Secondaries remember the attempt they joined; a failure
// is tagged with the attempt that produced it; `reset()` also bumps the
// generation.  A waiter whose attempt is no longer the current one
// (reset happened, or a newer primary started after its primary gave up)
// returns `.superseded` without touching state, so a stale waiter can
// never publish, re-arm, or report a failure into a newer attempt.

/// Result of one `ensureDownloaded` call.
enum FileDownloadWaitOutcome: Equatable {
    /// PROVIDE_DATA arrived for the current announcement with at least
    /// one path.  Carries the parsed newline-separated path list.
    case completed([String])
    /// `cancel()` was called while waiting (user hit Cancel on the panel).
    case cancelled
    /// The pending announcement was replaced mid-wait, or this waiter's
    /// attempt was replaced by a newer one (reset / newer primary); the
    /// reply, if it ever arrives, belongs to a different paste or attempt.
    case superseded
    /// No PROVIDE_DATA, no progress frame and no cancel for `timeout`
    /// seconds.
    case timedOut
    /// The semaphore fired but the payload could not be turned into a
    /// usable path list: announcement no longer current, no rendered
    /// data, bytes not UTF-8, or an EMPTY list — the parent replies with
    /// an empty payload precisely when its own download failed or
    /// stalled, so "zero files" is a failure, never a successful paste.
    case failed(String)
}

final class FileDownloadGate {

    /// Default stall bound: how long a paste may go without PROVIDE_DATA
    /// or a progress frame before it is given up.
    static let defaultTimeout: TimeInterval = 300

    /// How long `ensureDownloaded` yields to `idle` / sleeps between polls.
    static let defaultPollInterval: TimeInterval = 0.05

    let timeout: TimeInterval
    let pollInterval: TimeInterval

    private let condition = NSCondition()
    private var paths: [String]?
    private var triggered = false
    private var cancelled = false
    /// Most recent `noteProgress()` for the current announcement.
    private var lastProgressAt: Date?
    /// Attempt counter.  Bumped when a primary starts and on `reset()`.
    private var generation: UInt64 = 0
    /// Outcome of the last primary that gave up (`.timedOut` / `.failed`)
    /// together with the attempt it belonged to, handed to secondaries
    /// that joined that same attempt.  Cleared when a new primary starts
    /// and on `reset()`.
    private var lastFailure: (outcome: FileDownloadWaitOutcome, generation: UInt64)?

    init(timeout: TimeInterval = FileDownloadGate.defaultTimeout,
         pollInterval: TimeInterval = FileDownloadGate.defaultPollInterval) {
        self.timeout = timeout
        self.pollInterval = pollInterval
    }

    // MARK: State

    /// Forget everything about the previous announcement.  Called from
    /// `announceDelayedFiles` before the new items hit the pasteboard.
    /// Waiters still in flight for the old announcement see the
    /// generation change and return `.superseded`.
    func reset() {
        condition.lock()
        paths = nil
        triggered = false
        cancelled = false
        lastProgressAt = nil
        lastFailure = nil
        generation &+= 1
        condition.unlock()
    }

    /// Flip the cancel flag and wake any secondary waiter.  The primary
    /// waiter polls the flag between ticks; the caller is expected to also
    /// signal the render semaphore so the primary sees it promptly.
    func cancel() {
        condition.lock()
        cancelled = true
        condition.broadcast()
        condition.unlock()
    }

    /// Record that the parent reported download progress for the current
    /// announcement.  Pushes the stall deadline of every active wait out
    /// by `timeout` from now.
    func noteProgress() {
        condition.lock()
        lastProgressAt = Date()
        condition.unlock()
    }

    var isCancelled: Bool {
        condition.lock()
        defer { condition.unlock() }
        return cancelled
    }

    /// Path list published by a completed primary wait, if any.
    var cachedPaths: [String]? {
        condition.lock()
        defer { condition.unlock() }
        return paths
    }

    /// True while a primary caller owns the current attempt (it has sent
    /// DATA_REQUEST and either is still waiting or has published paths).
    /// False after `reset()` and after a primary gave up, so the next
    /// caller becomes a fresh primary.
    var hasTriggered: Bool {
        condition.lock()
        defer { condition.unlock() }
        return triggered
    }

    /// Current attempt number.  Exposed for tests; advances on every new
    /// primary and on `reset()`.
    var currentGeneration: UInt64 {
        condition.lock()
        defer { condition.unlock() }
        return generation
    }

    // MARK: Wait

    /// Coordinate the one-time download across potentially several
    /// concurrent data-provider callbacks (one per file).
    ///
    /// - Parameters:
    ///   - semaphore: The render semaphore that `PasteboardManager.provideData`
    ///     signals when PROVIDE_DATA arrives (and that `cancelFileDownload`
    ///     pokes so the primary wakes early).
    ///   - sendRequest: Sends DATA_REQUEST upstream.  Invoked exactly once
    ///     per attempt, by the primary caller, before it starts waiting.
    ///   - isStillCurrent: Whether the announcement this call was made for
    ///     is still the pending one.  Polled between ticks; a `false` ends
    ///     the wait as `.superseded`.
    ///   - renderedPayload: Reads the bytes `provideData` stored.  Only
    ///     consulted after the semaphore fires.
    ///   - idle: Called between polls to let the caller pump its run loop
    ///     (or sleep).  Should return after roughly `pollInterval`.
    func ensureDownloaded(
        semaphore: DispatchSemaphore,
        sendRequest: () -> Void,
        isStillCurrent: () -> Bool,
        renderedPayload: () -> Data?,
        idle: () -> Void
    ) -> FileDownloadWaitOutcome {
        condition.lock()

        // Already completed — return cached paths
        if let paths {
            condition.unlock()
            return .completed(paths)
        }

        if !triggered {
            triggered = true
            lastFailure = nil
            generation &+= 1
            let attempt = generation
            condition.unlock()
            return primaryWait(attempt: attempt,
                               semaphore: semaphore,
                               sendRequest: sendRequest,
                               isStillCurrent: isStillCurrent,
                               renderedPayload: renderedPayload,
                               idle: idle)
        }

        let joined = generation
        condition.unlock()
        return secondaryWait(joined: joined, isStillCurrent: isStillCurrent, idle: idle)
    }

    // MARK: Primary

    private func primaryWait(
        attempt: UInt64,
        semaphore: DispatchSemaphore,
        sendRequest: () -> Void,
        isStillCurrent: () -> Bool,
        renderedPayload: () -> Data?,
        idle: () -> Void
    ) -> FileDownloadWaitOutcome {
        // NOTE: the caller owns the "user actually pasted" UI signal
        // (onTransferStart); this gate only sends the request.  See the
        // rationale in PasteboardManager's data-provider callback.
        sendRequest()

        // Wait for PROVIDE_DATA while keeping the main run loop alive
        // (via `idle`).  Also polls the cancel flag so the user's Cancel
        // button click exits this loop promptly.
        let startedAt = Date()
        var completed = false
        var superseded = false
        var wasCancelled = false
        while !completed && !superseded && !wasCancelled && !hasStalled(since: startedAt) {
            if isCancelled {
                wasCancelled = true
            } else if semaphore.wait(timeout: .now()) == .success {
                // Re-check cancellation: the signal might have been
                // posted by cancelFileDownload (which also signals)
                // rather than by genuine PROVIDE_DATA completion.
                wasCancelled = isCancelled
                completed = !wasCancelled
            } else if !isStillCurrent() || !isCurrentAttempt(attempt) {
                superseded = true
            } else {
                idle()
            }
        }

        condition.lock()
        // A reset() while we were waiting means the announcement (or at
        // least the attempt) is gone: nothing we observed may be written
        // into the new attempt's state, and the caller must not act on it.
        guard generation == attempt else {
            condition.unlock()
            Log.warning("File download attempt superseded by reset while waiting")
            return .superseded
        }

        let outcome: FileDownloadWaitOutcome
        if completed {
            if !isStillCurrent() {
                outcome = .failed("announcement changed after PROVIDE_DATA")
            } else if let data = renderedPayload() {
                if let list = FileDownloadGate.parsePathList(data) {
                    if list.isEmpty {
                        outcome = .failed("parent replied with no file paths (its download failed or stalled)")
                    } else {
                        paths = list
                        outcome = .completed(list)
                        Log.info("File download complete: \(list.count) file(s)")
                    }
                } else {
                    outcome = .failed("PROVIDE_DATA payload is not UTF-8")
                }
            } else {
                outcome = .failed("PROVIDE_DATA signalled but no rendered data")
            }
        } else if wasCancelled {
            outcome = .cancelled
            Log.info("File download cancelled by user")
        } else if superseded {
            outcome = .superseded
            Log.warning("File download abandoned: announcement superseded mid-flight")
        } else {
            outcome = .timedOut
        }
        switch outcome {
        case .failed(let reason):
            Log.error("File download failed: \(reason)")
            // Re-arm: hand the failure to the secondaries of this attempt
            // and let the next caller start a fresh attempt.
            triggered = false
            lastFailure = (outcome, attempt)
        case .timedOut:
            Log.error("File download stalled: no PROVIDE_DATA or progress for \(timeout) s")
            triggered = false
            lastFailure = (outcome, attempt)
        default:
            break
        }
        condition.broadcast()
        condition.unlock()
        return outcome
    }

    // MARK: Secondary

    private func secondaryWait(
        joined: UInt64,
        isStillCurrent: () -> Bool,
        idle: () -> Void
    ) -> FileDownloadWaitOutcome {
        let startedAt = Date()
        while !hasStalled(since: startedAt) {
            condition.lock()
            let published = paths
            let wasCancelled = cancelled
            let current = generation
            let primaryGaveUp = !triggered
            let failure = lastFailure
            condition.unlock()
            if let published {
                return .completed(published)
            }
            if wasCancelled {
                Log.info("ensureFilesDownloaded (secondary): cancelled by user")
                return .cancelled
            }
            if current != joined {
                // The attempt we joined is over and a newer one (or a
                // reset) has taken its place.  Whatever happened to it is
                // that attempt's business; report nothing on its behalf.
                Log.warning("ensureFilesDownloaded (secondary): attempt superseded by a newer one, exiting")
                return .superseded
            }
            if primaryGaveUp {
                let outcome: FileDownloadWaitOutcome
                if let failure, failure.generation == joined {
                    outcome = failure.outcome
                } else {
                    outcome = .failed("primary wait gave up")
                }
                Log.warning("ensureFilesDownloaded (secondary): primary gave up (\(outcome))")
                return outcome
            }
            if !isStillCurrent() {
                Log.warning("ensureFilesDownloaded (secondary): announcement superseded, exiting")
                return .superseded
            }
            idle()
        }
        Log.error("Timed out waiting for file download (secondary)")
        return .timedOut
    }

    // MARK: Stall / attempt helpers

    /// True once more than `timeout` has passed since the later of
    /// `startedAt` and the most recent `noteProgress()`.  Progress recorded
    /// before this wait began does not extend its deadline.
    private func hasStalled(since startedAt: Date) -> Bool {
        condition.lock()
        let progress = lastProgressAt
        condition.unlock()
        var anchor = startedAt
        if let progress, progress > anchor {
            anchor = progress
        }
        return Date() >= anchor.addingTimeInterval(timeout)
    }

    private func isCurrentAttempt(_ attempt: UInt64) -> Bool {
        condition.lock()
        defer { condition.unlock() }
        return generation == attempt
    }

    // MARK: Parsing

    /// Turn a PROVIDE_DATA payload for a Files announcement into the path
    /// list the parent encoded: UTF-8, one absolute path per line, blank
    /// lines ignored.  Returns nil when the bytes are not valid UTF-8.
    /// An empty list is returned for an empty payload; the caller decides
    /// what that means (the gate treats it as a failed paste).
    static func parsePathList(_ data: Data) -> [String]? {
        guard let str = String(data: data, encoding: .utf8) else { return nil }
        return str.components(separatedBy: "\n").filter { !$0.isEmpty }
    }
}
