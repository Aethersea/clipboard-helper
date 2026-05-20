import AppKit
import Foundation
import UniformTypeIdentifiers

// MARK: - PasteboardManager + NSFilePromiseProviderDelegate
//
// Phase 1 scaffolding for plan-mac-virtual-file-clipboard.md. Provides the
// machinery needed to publish files via `NSFilePromiseProvider` alongside
// the existing `.fileURL` lazy provider, but does NOT yet wire it up in
// `announceDelayedFiles` — that's Phase 2.
//
// Threading model for the delegate callbacks:
//   * `fileNameForType` — called on the main thread by AppKit before
//     handing the promise to the destination process. Cheap lookup only;
//     do NOT block.
//   * `writePromiseTo` — called on the OperationQueue we return from
//     `operationQueue(for:)`. Synchronously kicks off the chunked
//     download into the OS-provided destination URL and waits on a
//     semaphore for completion. The OS gives us `completionHandler` to
//     fire exactly once when bytes have landed (or with an error). This
//     callback NEVER runs on the main thread, so unlike the legacy
//     `.fileURL` data-provider path we don't need to spin the run loop.
//   * `operationQueue(for:)` — called once per promise to discover where
//     `writePromiseTo` should run. We return a shared serial queue so the
//     per-announcement download ordering matches the announcement file
//     order — Finder's progress UI groups items by submission order.

extension PasteboardManager: NSFilePromiseProviderDelegate {

    // MARK: Factory

    /// Build a freshly-configured `NSFilePromiseProvider` for the i-th file
    /// in `announcement`. Caller (Phase 2 `announceDelayedFiles`) is
    /// responsible for adding the provider's pasteboard items to
    /// `NSPasteboard` and retaining the provider until the download
    /// completes — see `activePromiseProviders` (added in Phase 2).
    ///
    /// `userInfo` carries the cross-reference back to the announcement:
    ///   * `kPromiseUserInfoFileID`     — `FileMetadata.fileID`
    ///   * `kPromiseUserInfoTransferID` — `ClipboardAnnouncement.transferID`
    ///   * `kPromiseUserInfoFileSize`   — `FileMetadata.fileSize`
    ///   * `kPromiseUserInfoFilename`   — `FileMetadata.filename`
    /// The delegate methods read these instead of reaching into
    /// `pendingAnnouncement`, which can rotate between announce-time and
    /// paste-time when the user copies new content mid-flight.
    func makeFilePromiseProvider(
        for file: Leviathan_FileMetadata,
        transferID: String
    ) -> NSFilePromiseProvider {
        let uti = filePromiseUTI(forFilename: file.filename, mimeType: file.mimeType)
        let provider = NSFilePromiseProvider(fileType: uti, delegate: self)
        provider.userInfo = [
            kPromiseUserInfoFileID:     file.fileID,
            kPromiseUserInfoTransferID: transferID,
            kPromiseUserInfoFileSize:   file.fileSize,
            kPromiseUserInfoFilename:   file.filename
        ]
        return provider
    }

    // MARK: NSFilePromiseProviderDelegate

    func filePromiseProvider(
        _ filePromiseProvider: NSFilePromiseProvider,
        fileNameForType fileType: String
    ) -> String {
        let info = filePromiseProvider.userInfo as? [String: Any]
        let rawName = info?[kPromiseUserInfoFilename] as? String ?? ""
        if rawName.isEmpty {
            // The promise was built without a filename — sanitizeFilename
            // returns the stable "untitled" fallback below, but log the
            // miss separately so production deployments can correlate
            // bad announcements with paste failures.
            Log.warning("FilePromise: missing filename in userInfo; returning fallback")
        }
        return sanitizeFilename(rawName)
    }

    func filePromiseProvider(
        _ filePromiseProvider: NSFilePromiseProvider,
        writePromiseTo url: URL,
        completionHandler: @escaping (Error?) -> Void
    ) {
        // Phase 1: scaffolding only. The actual wiring into
        // FileTransferCoordinator happens here in Phase 2; for now we just
        // fail-fast so we don't silently swallow a paste if the new path
        // is accidentally activated before Phase 2 lands.
        //
        // We DO honor the completionHandler-exactly-once contract — calling
        // it with a definite error frees Finder's progress dialog instead
        // of leaving it spinning indefinitely.
        guard let info = filePromiseProvider.userInfo as? [String: Any],
              let fileID = info[kPromiseUserInfoFileID] as? String,
              let transferID = info[kPromiseUserInfoTransferID] as? String,
              let fileSize = info[kPromiseUserInfoFileSize] as? UInt64
        else {
            completionHandler(FilePromiseError.malformedUserInfo)
            return
        }

        // Snapshot the coordinator pointer once. `fileTransferCoordinator`
        // is an external `var` settable by AppDelegate at any time; in
        // practice it's set during applicationDidFinishLaunching well
        // before any pasteboard activity, but a defensive copy here
        // closes the data-race window between AppDelegate (re-)assigning
        // the property on the main thread and this method reading it on
        // the file-promise OperationQueue.
        guard let coordinator = fileTransferCoordinator else {
            Log.error("FilePromise: writePromiseTo \(url.lastPathComponent) but coordinator missing")
            completionHandler(FilePromiseError.coordinatorUnavailable)
            return
        }

        // System-discoverable Progress for the Finder copy panel.
        //
        // Because clipboard-helper runs as `LSUIElement` (background
        // daemon, no dock icon), Finder can't infer file-write progress
        // by observing our app — it needs an explicitly published
        // Progress that names the destination URL and the operation
        // kind.  Two API details matter:
        //
        //   * The URL + kind have to go through
        //     setUserInfoObject(_:forKey:), NOT the `progress.fileURL` /
        //     `progress.fileOperationKind` convenience setters.  The
        //     convenience setters only propagate the value when Progress
        //     has been initialised via `Progress(parent:userInfo:)`;
        //     when we use `Progress(totalUnitCount:)` the system reads
        //     the userInfo dictionary directly.
        //   * `.downloading` is the right operation kind for a remote
        //     stream (chunks arriving over WebRTC), not `.receiving`
        //     (which is for "Save / Receive" semantics in Mail-style
        //     apps).
        //
        // Async return is preserved — writePromiseTo's contract is to
        // dispatch work and call completionHandler asynchronously.
        // Blocking the operation queue thread here cripples Finder's
        // XPC-driven progress UI (it expects writePromiseTo to return
        // promptly so the in-progress Progress is the one driving the
        // panel).
        let progress = Progress(totalUnitCount: Int64(max(fileSize, 1)))
        progress.kind = .file
        progress.setUserInfoObject(url, forKey: .fileURLKey)
        progress.setUserInfoObject(
            Progress.FileOperationKind.downloading,
            forKey: .fileOperationKindKey)
        progress.isCancellable = false
        progress.isPausable = false
        progress.publish()

        // Wrap the OS-provided completion so we always unpublish before
        // the system marks the file done.  Skipping unpublish leaves a
        // ghost progress bar in the Finder copy panel after the actual
        // write has finished.
        let wrappedCompletion: (Error?) -> Void = { error in
            progress.unpublish()
            completionHandler(error)
        }

        Log.info("FilePromise: writePromiseTo \(url.path) fileID=\(fileID) bytes=\(fileSize)")
        coordinator.startFileDownload(
            transferID: transferID,
            fileID: fileID,
            fileSize: fileSize,
            to: url,
            parentProgress: progress,
            completion: wrappedCompletion
        )
    }

    func operationQueue(
        for filePromiseProvider: NSFilePromiseProvider
    ) -> OperationQueue {
        return Self.sharedFilePromiseQueue
    }

    // MARK: Shared serial queue for writePromiseTo callbacks
    //
    // One OperationQueue per process. Serial (maxConcurrentOperationCount=1)
    // so a 100-file paste serializes its `writePromiseTo` callbacks in the
    // order Finder requested them — matches the announcement file order
    // and keeps FILE_CHUNK_REQUEST round-trips predictable for the
    // upstream pipe. The per-file download itself is pipelined inside
    // `FileTransferCoordinator.startFileDownload` (kPipelineDepth=4 chunks
    // in flight per file), so serializing the outer dispatch doesn't
    // sacrifice per-file throughput.
    private static let sharedFilePromiseQueue: OperationQueue = {
        let q = OperationQueue()
        q.name = "com.leviathan.clipboard-helper.file-promise"
        q.qualityOfService = .userInitiated
        q.maxConcurrentOperationCount = 1
        return q
    }()
}

// MARK: - UTI lookup

/// Sanitize a remote-announced filename into a basename safe to hand
/// the OS. Strips any path components so a malicious or accidental
/// `relative_path = "../../etc/passwd"` cannot influence WHERE the OS
/// writes the file — AppKit honors only the basename anyway, but
/// defense-in-depth keeps the contract explicit on our side.
///
/// Returns "untitled" when the input is empty OR when the basename
/// after stripping is empty (e.g. trailing-slash paths like "a/b/").
///
/// Internal to the file so unit tests can exercise it without
/// instantiating an NSFilePromiseProvider. Pure: no I/O, no global
/// state.
func sanitizeFilename(_ rawName: String) -> String {
    guard !rawName.isEmpty else { return "untitled" }
    let basename = (rawName as NSString).lastPathComponent
    return basename.isEmpty ? "untitled" : basename
}

/// Resolve a UTI for an NSFilePromiseProvider given the announcement's
/// filename + MIME hint. Strategy:
///   1. If `filename` has an extension, ask `UTType` to map it; this is
///      what Finder uses to pick the icon and infer the receiving app.
///   2. Else if `mimeType` is non-empty, ask `UTType` for a MIME match.
///   3. Else fall back to `public.data` — a documented "I'm a file, I
///      don't know what kind" UTI that still triggers Finder's generic
///      file-paste handling.
///
/// Internal to the file so it can be unit-tested without touching the
/// pasteboard. Pure: no I/O, no global state.
func filePromiseUTI(forFilename filename: String, mimeType: String) -> String {
    let ext = (filename as NSString).pathExtension
    if !ext.isEmpty,
       let t = UTType(filenameExtension: ext),
       !t.identifier.isEmpty {
        return t.identifier
    }
    if !mimeType.isEmpty,
       let t = UTType(mimeType: mimeType),
       !t.identifier.isEmpty {
        return t.identifier
    }
    return UTType.data.identifier
}

// MARK: - Errors

enum FilePromiseError: Error, LocalizedError {
    case malformedUserInfo
    case coordinatorUnavailable

    var errorDescription: String? {
        switch self {
        case .malformedUserInfo:
            return "NSFilePromiseProvider.userInfo was missing the expected file-id / transfer-id / size keys"
        case .coordinatorUnavailable:
            return "FileTransferCoordinator was not attached to PasteboardManager"
        }
    }
}

// MARK: - userInfo keys

let kPromiseUserInfoFileID     = "com.leviathan.clipboard.promise.fileID"
let kPromiseUserInfoTransferID = "com.leviathan.clipboard.promise.transferID"
let kPromiseUserInfoFileSize   = "com.leviathan.clipboard.promise.fileSize"
let kPromiseUserInfoFilename   = "com.leviathan.clipboard.promise.filename"
