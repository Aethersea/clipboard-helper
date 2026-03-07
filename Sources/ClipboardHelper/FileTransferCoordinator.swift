import AppKit
import Foundation

/// Default chunk size: 256 KB — balances IPC frame size vs. round-trip overhead.
private let kDefaultChunkSize: UInt32 = 256 * 1024

// MARK: - FileTransferCoordinator

/// Coordinates file downloads from the parent process using chunked IPC transfers.
///
/// Flow (client mode — receiving files from remote server via shen/Electron):
///   1. `registerAnnouncement(_:)` is called when ANNOUNCE_DELAYED arrives with files.
///   2. `PasteboardManager` writes `NSFilePromiseProvider` objects to the pasteboard.
///   3. When the user drops/pastes into Finder, `startFileDownload(...)` is called.
///   4. We send sequential `FILE_CHUNK_REQUEST` messages to the parent process.
///   5. The parent responds with `FILE_CHUNK_DATA` messages; we write them to disk.
///   6. `NSProgress` is updated each chunk — Finder shows the native progress dialog.
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
        let task = FileDownloadTask(
            transferID: transferID,
            fileID: fileID,
            fileSize: fileSize,
            destURL: destURL,
            parentProgress: parentProgress,
            completion: completion
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
    func handleChunkData(_ chunk: Leviathan_HelperFileChunkData) {
        let key = "\(chunk.transferID)/\(chunk.fileID)"
        queue.async { [weak self] in
            guard let self else { return }
            let task = self.downloads[key]
            if chunk.isLast || !chunk.error.isEmpty {
                self.downloads.removeValue(forKey: key)
            }
            task?.handleChunkData(chunk)
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

/// Manages a single file download: sends chunk requests, writes data, reports progress.
private final class FileDownloadTask {
    let transferID: String
    let fileID: String
    let fileSize: UInt64
    let destURL: URL
    let completion: (Error?) -> Void

    private let progress: Progress
    private var fileHandle: FileHandle?
    private var receivedBytes: UInt64 = 0
    private var isCancelled = false
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

        // Create (or truncate) the destination file
        FileManager.default.createFile(atPath: destURL.path, contents: nil)
        guard let fh = try? FileHandle(forWritingTo: destURL) else {
            completion(FileDownloadError.createFailed(destURL.path))
            return
        }
        fileHandle = fh

        Log.info("[FileTransfer] Starting download → \(destURL.lastPathComponent) (\(fileSize) B)")
        sendRequest(0)
    }

    func handleChunkData(_ chunk: Leviathan_HelperFileChunkData) {
        guard !isCancelled else { return }

        if !chunk.error.isEmpty {
            finish(error: FileDownloadError.serverError(chunk.error))
            return
        }

        do {
            try fileHandle?.seek(toOffset: chunk.offset)
            fileHandle?.write(chunk.data)
        } catch {
            finish(error: FileDownloadError.writeFailed(error.localizedDescription))
            return
        }

        receivedBytes += UInt64(chunk.data.count)
        progress.completedUnitCount = Int64(receivedBytes)

        if chunk.isLast {
            finish(error: nil)
        } else {
            let nextOffset = chunk.offset + UInt64(chunk.data.count)
            sendRequest?(nextOffset)
        }
    }

    func cancel() {
        isCancelled = true
        finish(error: FileDownloadError.cancelled)
    }

    private func finish(error: Error?) {
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
