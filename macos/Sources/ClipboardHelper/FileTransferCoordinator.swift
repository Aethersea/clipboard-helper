import AppKit
import Foundation

/// Default chunk size: 256 KB — balances IPC frame size vs. round-trip overhead.
private let kDefaultChunkSize: UInt32 = 256 * 1024

/// Maximum number of FILE_CHUNK_REQUEST messages with no FILE_CHUNK_DATA
/// reply yet — the pipeline depth that hides the per-chunk Unix-socket
/// round trip.  4 × 256 KB = 1 MB of in-flight data per file, which
/// tracks well with macOS pasteboard transfer rates without stressing
/// the helper's process-wide socket frame buffer when many files are
/// downloaded in parallel.  Strict serial (depth=1) was the previous
/// behaviour and made any non-trivial file feel like it was throttled
/// to RTT-bound throughput; depth=4 reaches steady-state TCP-like
/// throughput on a Unix socket without unbounded buffering.
private let kPipelineDepth: Int = 4

// MARK: - FileTransferCoordinator

/// Coordinates file downloads from the parent process using chunked IPC transfers.
///
/// Flow (client mode — receiving files from remote server via shen/Electron):
///   1. `registerAnnouncement(_:)` is called when ANNOUNCE_DELAYED arrives with files.
///   2. `PasteboardManager` writes `NSFilePromiseProvider` objects to the pasteboard.
///   3. When the user drops/pastes into Finder, `startFileDownload(...)` is called.
///   4. We send up to `kPipelineDepth` `FILE_CHUNK_REQUEST` messages concurrently
///      to the parent process — pipelined to hide IPC round-trip latency.
///   5. The parent responds with `FILE_CHUNK_DATA` messages (possibly out of
///      order with respect to the original request order); we seek + write
///      each one to its own offset.
///   6. `NSProgress` is updated each chunk — Finder shows the native progress dialog.
///   7. The download finishes when `receivedBytes == fileSize`, NOT when an
///      `isLast=true` chunk arrives — pipelined replies can deliver the
///      terminal chunk before earlier ones complete.
///
/// Flow (server mode — serving local files to the parent Go process):
///   The clipboard-helper itself owns the local file URLs (from `readPasteboard`).
///   When `FILE_CHUNK_REQUEST` arrives from the Go parent, `serveLocalChunk(...)` is called
///   directly by `AppDelegate`; this coordinator is not involved for server-mode serving.
final class FileTransferCoordinator {

    // MARK: - Outgoing message hook

    /// Set by AppDelegate to actually send a message to the parent process.
    var sendMessage: ((Leviathan_HelperMessage) -> Void)?

    // MARK: - State

    private struct AnnouncementInfo {
        let contentHash: String
        var files: [String: Leviathan_FileMetadata]  // file_id → metadata
    }

    // transferID → info; populated when ANNOUNCE_DELAYED arrives
    private var announcements: [String: AnnouncementInfo] = [:]

    // "transferID/fileID" → active download task
    private var downloads: [String: FileDownloadTask] = [:]

    private let queue = DispatchQueue(
        label: "com.leviathan.clipboard-helper.filetransfer",
        qos: .userInitiated
    )

    // MARK: - Registration

    /// Register an incoming announcement so we know which files belong to which transfer.
    func registerAnnouncement(_ announcement: Leviathan_ClipboardAnnouncement) {
        var fileMap: [String: Leviathan_FileMetadata] = [:]
        for file in announcement.files {
            fileMap[file.fileID] = file
        }
        let tid = announcement.transferID
        queue.async {
            self.announcements[tid] = AnnouncementInfo(
                contentHash: announcement.contentHash,
                files: fileMap
            )
        }
        Log.info("[FileTransfer] Registered transfer \(tid) — \(announcement.files.count) file(s)")
    }

    /// Return file metadata for a given transfer and file.
    func fileMetadata(transferID: String, fileID: String) -> Leviathan_FileMetadata? {
        var result: Leviathan_FileMetadata?
        queue.sync { result = announcements[transferID]?.files[fileID] }
        return result
    }

    // MARK: - Download

    /// Start downloading a file from the parent process.
    ///
    /// - Parameters:
    ///   - transferID: Matches the transfer_id in the `ClipboardAnnouncement`.
    ///   - fileID: The specific file to download.
    ///   - fileSize: Expected file size in bytes (used for progress).
    ///   - destURL: Where macOS wants the file written (from `NSFilePromiseProvider`).
    ///   - parentProgress: A `Progress` object hierarchy; a child is added here so
    ///                     Finder's "Copying" progress dialog tracks this file.
    ///   - completion: Called when done (nil = success, non-nil = error).
    func startFileDownload(
        transferID: String,
        fileID: String,
        fileSize: UInt64,
        to destURL: URL,
        parentProgress: Progress,
        completion: @escaping (Error?) -> Void
    ) {
        let key = "\(transferID)/\(fileID)"

        // Wrap the caller's completion so the task is removed from the
        // downloads dict when (and ONLY when) FileDownloadTask itself
        // decides it's done.  Removing on `chunk.isLast` is wrong under
        // pipelining: the terminal chunk can arrive before earlier
        // ones, and dropping the task there would discard the still-
        // in-flight chunks and leave holes on disk.
        let wrappedCompletion: (Error?) -> Void = { [weak self] error in
            self?.queue.async { [weak self] in
                self?.downloads.removeValue(forKey: key)
            }
            completion(error)
        }

        let task = FileDownloadTask(
            transferID: transferID,
            fileID: fileID,
            fileSize: fileSize,
            destURL: destURL,
            parentProgress: parentProgress,
            completion: wrappedCompletion
        )

        queue.async { [weak self] in
            guard let self else { return }
            self.downloads[key] = task
            task.start { [weak self] offset in
                self?.sendChunkRequest(transferID: transferID, fileID: fileID, offset: offset)
            }
        }
    }

    // MARK: - Incoming chunk data

    /// Handle a `FILE_CHUNK_DATA` message arriving from the parent process.
    ///
    /// Does NOT inspect `chunk.isLast` — under the pipelined download
    /// model the terminal chunk can arrive before earlier chunks are
    /// processed, so the chunk flag is unreliable as a "task complete"
    /// signal.  The task is removed from the dict only via the wrapped
    /// completion in `startFileDownload`, which fires when the task's
    /// own `finish()` runs (after all in-flight requests have been
    /// reconciled).
    func handleChunkData(_ chunk: Leviathan_HelperFileChunkData) {
        let key = "\(chunk.transferID)/\(chunk.fileID)"
        queue.async { [weak self] in
            guard let self else { return }
            self.downloads[key]?.handleChunkData(chunk)
        }
    }

    // MARK: - Cancel

    /// Cancel all active downloads for a transfer (e.g. connection dropped).
    func cancelTransfer(_ transferID: String) {
        queue.async { [weak self] in
            guard let self else { return }
            let keysToRemove = self.downloads.keys.filter { $0.hasPrefix("\(transferID)/") }
            for key in keysToRemove {
                self.downloads[key]?.cancel()
                self.downloads.removeValue(forKey: key)
            }
            self.announcements.removeValue(forKey: transferID)
        }
    }

    // MARK: - Private helpers

    private func sendChunkRequest(transferID: String, fileID: String, offset: UInt64) {
        var req = Leviathan_HelperFileChunkRequest()
        req.transferID = transferID
        req.fileID = fileID
        req.offset = offset
        req.size = kDefaultChunkSize

        var msg = Leviathan_HelperMessage()
        msg.type = .fileChunkRequest
        msg.payload = .fileChunkRequest(req)
        msg.timestamp = UInt64(Date().timeIntervalSince1970 * 1000)

        sendMessage?(msg)
        Log.debug("[FileTransfer] → FILE_CHUNK_REQUEST transferID=\(transferID) fileID=\(fileID) offset=\(offset)")
    }
}

// MARK: - FileDownloadTask

private enum FileDownloadError: Error, LocalizedError {
    case createFailed(String)
    case writeFailed(String)
    case serverError(String)
    case cancelled

    var errorDescription: String? {
        switch self {
        case .createFailed(let m): return "Cannot create file: \(m)"
        case .writeFailed(let m): return "Write failed: \(m)"
        case .serverError(let m): return "Server error: \(m)"
        case .cancelled: return "Transfer cancelled"
        }
    }
}

/// Manages a single file download: pipelines chunk requests, writes data,
/// reports progress.
///
/// Concurrency model: every method on this class is invoked on the
/// FileTransferCoordinator's serial dispatch queue.  Mutations to
/// `inFlight`, `nextRequestOffset`, `receivedBytes`, `isCancelled`,
/// `isFinished`, and `fileHandle` are therefore safe without explicit
/// locking.  The completion callback is hopped to the main queue so
/// callers don't have to be queue-aware.
private final class FileDownloadTask {
    let transferID: String
    let fileID: String
    let fileSize: UInt64
    let destURL: URL
    let completion: (Error?) -> Void

    private let progress: Progress
    private var fileHandle: FileHandle?
    private var receivedBytes: UInt64 = 0

    /// Offset of the next FILE_CHUNK_REQUEST we'll send.  Advances by
    /// exactly `kDefaultChunkSize` per request — the server may return
    /// a smaller terminal chunk for the tail of the file, but never
    /// overshoots, so request offsets are always in step with the
    /// chunked grid.
    private var nextRequestOffset: UInt64 = 0

    /// FILE_CHUNK_REQUESTs sent without a FILE_CHUNK_DATA reply yet.
    /// Bounded by `kPipelineDepth` so the helper doesn't enqueue
    /// unbounded work into the parent's IPC frame buffer.
    private var inFlight: Int = 0

    private var isCancelled = false

    /// Idempotency guard for `finish` — both `handleChunkData` (success
    /// path) and `cancel` (external interruption) can race to invoke
    /// `finish`, and without this flag the completion callback would
    /// fire twice and `closeFile` would be called on a torn-down
    /// handle.  Set true on first entry; subsequent calls are no-ops.
    private var isFinished = false

    private var sendRequest: ((UInt64) -> Void)?

    init(
        transferID: String,
        fileID: String,
        fileSize: UInt64,
        destURL: URL,
        parentProgress: Progress,
        completion: @escaping (Error?) -> Void
    ) {
        self.transferID = transferID
        self.fileID = fileID
        self.fileSize = fileSize
        self.destURL = destURL
        self.completion = completion

        // Create a child progress so Finder's progress dialog sums all files
        progress = Progress(totalUnitCount: Int64(max(fileSize, 1)))
        parentProgress.addChild(progress, withPendingUnitCount: Int64(max(fileSize, 1)))
    }

    func start(sendRequest: @escaping (UInt64) -> Void) {
        self.sendRequest = sendRequest

        Log.info("[FileTransfer] Starting download → \(destURL.lastPathComponent) (\(fileSize) B, pipeline=\(kPipelineDepth))")

        // Touch the destination path.  NSFilePromiseProvider promised
        // this URL to Finder before any chunks were requested, so the
        // file must exist (even at 0 bytes) by the time we report
        // success.  FileHandle(forWritingTo:) below also requires the
        // path to already exist.
        FileManager.default.createFile(atPath: destURL.path, contents: nil)

        // Empty file: no IPC round trip needed; the createFile above
        // is sufficient to satisfy the promise.
        if fileSize == 0 {
            finish(error: nil)
            return
        }

        // Open for writing.  Cleaning up the 0-byte stub we just
        // touched on open-failure prevents the user from seeing an
        // orphan empty file in Finder when the helper hits a sandbox
        // or permission error during a download attempt.
        guard let fh = try? FileHandle(forWritingTo: destURL) else {
            try? FileManager.default.removeItem(at: destURL)
            finish(error: FileDownloadError.createFailed(destURL.path))
            return
        }
        fileHandle = fh

        primeRequests()
    }

    /// Top up the in-flight pipeline.  Called once at start to fill the
    /// initial window, and again after every successful chunk to keep
    /// the window full as long as there's more file to fetch.  No-op
    /// once the entire file has been requested.
    private func primeRequests() {
        guard let sendRequest else { return }
        while inFlight < kPipelineDepth && nextRequestOffset < fileSize {
            sendRequest(nextRequestOffset)
            // Plain `+=` on UInt64 traps on overflow.  Wrapping
            // (.partialValue) would silently restart from byte 0 and
            // corrupt the file in any unlikely failure scenario; for
            // a clipboard helper a crash is preferable to data
            // corruption.  Hitting overflow requires a >18 EB file —
            // not realistic — so the trap is purely defensive.
            nextRequestOffset += UInt64(kDefaultChunkSize)
            inFlight += 1
        }
    }

    func handleChunkData(_ chunk: Leviathan_HelperFileChunkData) {
        guard !isCancelled, !isFinished else { return }

        // Decrement in-flight regardless of error vs success — the server
        // returned a frame for one of our outstanding requests.
        if inFlight > 0 { inFlight -= 1 }

        if !chunk.error.isEmpty {
            finish(error: FileDownloadError.serverError(chunk.error))
            return
        }

        // Silent-truncation defense.  An empty chunk with no error AND no
        // legitimate "we're at EOF" reason would otherwise be silently
        // accepted: receivedBytes wouldn't grow, but nextRequestOffset had
        // already been advanced by primeRequests, so the pipeline would
        // keep firing requests for offsets past where the server has
        // data; eventually `nextRequestOffset >= fileSize && inFlight==0`
        // and finish(nil) would commit a sparse / truncated file as a
        // success.  We've actually seen the equivalent failure mode in
        // the Windows IDataObject path (announcement size disagreed with
        // remote file size).  Treating empty mid-stream replies as a
        // truncation error fails the paste loudly instead of producing
        // a wrong-content file.
        //
        // "Legitimate empty" cases:
        //   * Past-EOF read — only possible if the server (or chunk.offset
        //     calculation) drifted; primeRequests is bounded by fileSize
        //     so we shouldn't reach this on a healthy stream.
        //   * Zero-byte file — fileSize would be 0, in which case
        //     startFileDownload short-circuits before primeRequests runs;
        //     handleChunkData isn't reached.
        if chunk.data.isEmpty {
            Log.warning(
                "[FileTransfer] Empty FILE_CHUNK_DATA at offset=\(chunk.offset) " +
                "(fileSize=\(fileSize), received=\(receivedBytes)) — " +
                "treating as truncation error to avoid silent sparse-file completion"
            )
            finish(error: FileDownloadError.serverError(
                "empty chunk reply at offset \(chunk.offset) (truncation)"))
            return
        }

        do {
            // seek + write must be on the same queue (they are: the
            // coordinator queue serializes all FileDownloadTask calls).
            // `write(contentsOf:)` is the modern throwing variant — the
            // legacy `write(_:)` swallows disk-full and other I/O errors
            // by crashing the process, which is unacceptable for a
            // helper that needs to report failures back to the parent.
            try fileHandle?.seek(toOffset: chunk.offset)
            try fileHandle?.write(contentsOf: chunk.data)
        } catch {
            finish(error: FileDownloadError.writeFailed(error.localizedDescription))
            return
        }

        receivedBytes += UInt64(chunk.data.count)
        progress.completedUnitCount = Int64(receivedBytes)

        // Done when *both*: every byte has been requested, and every
        // outstanding request has been answered.  Trusting
        // `chunk.isLast` would close the file mid-write because
        // pipelined replies can deliver the terminal chunk before
        // earlier ones complete.  Trusting `receivedBytes == fileSize`
        // would falsely succeed if the server ever duplicated a chunk
        // (e.g. internal retransmit) — `receivedBytes` would hit
        // `fileSize` while the on-disk byte range still has a hole.
        // The (offset, in-flight) pair is the source of truth: every
        // chunk grid slot has been requested AND every reply has been
        // processed.
        if nextRequestOffset >= fileSize && inFlight == 0 {
            finish(error: nil)
            return
        }

        // Otherwise top up the pipeline so we always have kPipelineDepth
        // requests outstanding until we've requested the whole file.
        primeRequests()
    }

    func cancel() {
        isCancelled = true
        finish(error: FileDownloadError.cancelled)
    }

    private func finish(error: Error?) {
        // Idempotent: handleChunkData(isLast) and cancel() can both reach
        // here for the same task.  Without this guard, the completion
        // callback would fire twice (once with success, once with
        // cancelled), and the second closeFile would target an already-
        // torn-down handle.
        if isFinished { return }
        isFinished = true

        fileHandle?.closeFile()
        fileHandle = nil
        if error == nil {
            Log.info("[FileTransfer] Download complete: \(destURL.lastPathComponent)")
            progress.completedUnitCount = progress.totalUnitCount
        } else {
            Log.error("[FileTransfer] Download failed: \(destURL.lastPathComponent) — \(error!.localizedDescription)")
        }
        DispatchQueue.main.async { [completion] in
            completion(error)
        }
    }
}
