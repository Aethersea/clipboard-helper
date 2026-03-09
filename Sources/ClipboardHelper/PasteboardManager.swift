import AppKit
import Foundation
import CommonCrypto

/// Manages NSPasteboard interactions: direct set, delayed rendering, and polling.
/// Must run on the main thread (NSApplication run loop).
final class PasteboardManager: NSObject {
    private let pasteboard = NSPasteboard.general
    private var lastChangeCount: Int
    private var pollTimer: DispatchSourceTimer?
    private let pollInterval: TimeInterval

    // Delayed rendering state
    private var pendingAnnouncement: Leviathan_ClipboardAnnouncement?
    private var renderSemaphore = DispatchSemaphore(value: 0)
    private var renderedData: Data?
    private var lastSetHash: String?

    // File transfer coordinator (injected by AppDelegate in client mode)
    var fileTransferCoordinator: FileTransferCoordinator?

    // Local file URLs from the last pasteboard read, keyed by file_id.
    // Used in server mode so AppDelegate can serve FILE_CHUNK_REQUESTs.
    private(set) var localFileURLs: [String: URL] = [:]

    // Callbacks
    var onClipboardChanged: ((Leviathan_ClipboardData) -> Void)?
    var onDataRequest: ((Leviathan_ClipboardDataRequest) -> Void)?

    // File download coordination (for NSPasteboardItemDataProvider)
    private var fileDownloadPaths: [String]?
    private var fileDownloadTriggered = false
    private let fileDownloadCondition = NSCondition()

    init(pollInterval: TimeInterval = 0.5) {
        self.pollInterval = pollInterval
        self.lastChangeCount = NSPasteboard.general.changeCount
        super.init()
    }

    deinit {
        stopPolling()
    }

    // MARK: - Polling

    func startPolling() {
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + pollInterval, repeating: pollInterval)
        timer.setEventHandler { [weak self] in
            self?.checkForChanges()
        }
        pollTimer = timer
        timer.resume()
        Log.info("Pasteboard polling started (interval: \(pollInterval)s)")
    }

    func stopPolling() {
        pollTimer?.cancel()
        pollTimer = nil
    }

    // MARK: - Set Clipboard (direct)

    func setClipboard(_ data: Leviathan_ClipboardData) {
        assert(Thread.isMainThread)

        // Remember hash to suppress echo
        lastSetHash = data.contentHash

        pasteboard.clearContents()

        switch data.contentType {
        case .text:
            if let text = String(data: data.payload, encoding: .utf8) {
                pasteboard.setString(text, forType: .string)
                Log.info("Set clipboard: text (\(text.count) chars)")
            }

        case .image:
            // Payload is WebP — decode to NSImage and write multiple formats
            if let image = NSImage(data: data.payload) {
                pasteboard.writeObjects([image])
                Log.info("Set clipboard: image (\(data.payload.count) bytes)")
            } else {
                // Fallback: write raw data as PNG type
                pasteboard.setData(data.payload, forType: .png)
                Log.info("Set clipboard: image raw (\(data.payload.count) bytes)")
            }

        case .files:
            // Write file URLs from paths provided in file metadata
            var urls: [NSURL] = []
            for file in data.files {
                // The path may come via filename (full path from server) or relativePath
                let path = !file.relativePath.isEmpty ? file.relativePath : file.filename
                guard !path.isEmpty else { continue }
                let url = NSURL(fileURLWithPath: path)
                if FileManager.default.fileExists(atPath: path) {
                    urls.append(url)
                    Log.info("Set clipboard: file \(path)")
                } else {
                    Log.warning("Set clipboard: file not found at \(path)")
                }
            }
            if !urls.isEmpty {
                pasteboard.writeObjects(urls)
                Log.info("Set clipboard: \(urls.count) file(s)")
            } else {
                Log.warning("Set clipboard: no valid file URLs to write")
            }

        default:
            Log.warning("Unknown content type: \(data.contentType)")
        }

        lastChangeCount = pasteboard.changeCount
    }

    // MARK: - Announce Delayed (lazy rendering)

    func announceDelayed(_ announcement: Leviathan_ClipboardAnnouncement) {
        assert(Thread.isMainThread)

        pendingAnnouncement = announcement
        lastSetHash = announcement.contentHash
        renderedData = nil

        var types: [NSPasteboard.PasteboardType] = []
        switch announcement.contentType {
        case .text:
            types = [.string]
        case .image:
            types = [.png, .tiff]
        case .files:
            // Files use NSPasteboardItemDataProvider — handled separately below
            break
        default:
            break
        }

        if announcement.contentType == .files {
            announceDelayedFiles(announcement)
            return
        }

        guard !types.isEmpty else {
            Log.warning("No types to declare for announcement")
            return
        }

        // declareTypes:owner: — macOS will call pasteboard:provideDataForType: when user pastes
        pasteboard.declareTypes(types, owner: self)
        lastChangeCount = pasteboard.changeCount

        Log.info("Announced delayed: \(announcement.contentType) hash=\(announcement.contentHash)")
    }

    private func announceDelayedFiles(_ announcement: Leviathan_ClipboardAnnouncement) {
        assert(Thread.isMainThread)

        guard !announcement.files.isEmpty else {
            Log.warning("Announce files: empty file list")
            return
        }

        // Store announcement so the data provider callback can reference it
        pendingAnnouncement = announcement
        lastSetHash = announcement.contentHash
        renderedData = nil

        // Reset file download state
        fileDownloadCondition.lock()
        fileDownloadPaths = nil
        fileDownloadTriggered = false
        fileDownloadCondition.unlock()

        // Drain any stale semaphore signals from previous operations
        while renderSemaphore.wait(timeout: .now()) == .success {}

        // Create one NSPasteboardItem per file with lazy data provider for .fileURL.
        // When the user pastes (Cmd+V), macOS calls the NSPasteboardItemDataProvider
        // callback which triggers the on-demand download from the remote peer.
        var items: [NSPasteboardItem] = []
        for (i, _) in announcement.files.enumerated() {
            let item = NSPasteboardItem()
            // Store file index as immediate data on a custom type
            item.setString("\(i)", forType: .init("com.leviathan.clipboard.file-index"))
            // Register lazy data provider for file URL
            item.setDataProvider(self, forTypes: [.fileURL])
            items.append(item)
        }

        pasteboard.clearContents()
        pasteboard.writeObjects(items)
        lastChangeCount = pasteboard.changeCount

        Log.info("Announced delayed files: \(announcement.files.count) file(s) hash=\(announcement.contentHash)")
    }

    // MARK: - Provide Data (response from parent)

    func provideData(_ data: Leviathan_HelperProvideData) {
        renderedData = data.data
        renderSemaphore.signal()
        Log.info("Data provided: \(data.data.count) bytes for hash=\(data.contentHash)")
    }

    // MARK: - Get Current Clipboard

    func getCurrentClipboard() -> Leviathan_ClipboardData {
        assert(Thread.isMainThread)
        return readPasteboard()
    }

    // MARK: - NSPasteboard Owner (delayed rendering callbacks)

    @objc func pasteboard(_ sender: NSPasteboard, provideDataForType type: NSPasteboard.PasteboardType) {
        guard let announcement = pendingAnnouncement else {
            Log.warning("provideDataForType called but no pending announcement")
            return
        }

        Log.info("OS requested data for type: \(type.rawValue)")

        // If we already have rendered data (cached from a previous format request for the same paste),
        // provide it directly
        if let cached = renderedData {
            writeDataToPasteboard(cached, type: type, announcement: announcement)
            return
        }

        // Send DATA_REQUEST to parent
        var request = Leviathan_ClipboardDataRequest()
        request.contentHash = announcement.contentHash
        request.contentType = announcement.contentType
        onDataRequest?(request)

        // Block waiting for PROVIDE_DATA response (timeout 30s)
        let result = renderSemaphore.wait(timeout: .now() + 30)

        if result == .timedOut {
            Log.error("Timed out waiting for data from parent (type=\(announcement.contentType))")
            return
        }

        guard let data = renderedData else {
            Log.error("No data received after signal")
            return
        }

        writeDataToPasteboard(data, type: type, announcement: announcement)
    }

    @objc func pasteboardChangedOwner(_ sender: NSPasteboard) {
        Log.info("Pasteboard ownership lost")
        pendingAnnouncement = nil
        renderedData = nil
    }

    // MARK: - Private

    private func checkForChanges() {
        let currentCount = pasteboard.changeCount
        guard currentCount != lastChangeCount else { return }
        lastChangeCount = currentCount

        let data = readPasteboard()

        // Suppress echo: don't report changes we caused ourselves
        if let lastHash = lastSetHash, data.contentHash == lastHash {
            return
        }

        guard data.contentType != .unspecified else { return }

        Log.info("Local clipboard changed: \(data.contentType) hash=\(data.contentHash)")
        onClipboardChanged?(data)
    }

    private func readPasteboard() -> Leviathan_ClipboardData {
        var data = Leviathan_ClipboardData()

        // Try file URLs first — files on clipboard also have text/image representations,
        // so this must be checked before text to avoid misclassifying file copies as text.
        if let urls = pasteboard.readObjects(forClasses: [NSURL.self], options: [
            .urlReadingFileURLsOnly: true
        ]) as? [URL], !urls.isEmpty {
            data.contentType = .files
            localFileURLs = [:]  // reset and repopulate
            for (i, url) in urls.enumerated() {
                var meta = Leviathan_FileMetadata()
                meta.fileID = "\(i)"
                meta.filename = url.lastPathComponent
                meta.relativePath = url.path
                if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path) {
                    meta.fileSize = (attrs[.size] as? UInt64) ?? 0
                }
                meta.isDirectory = url.hasDirectoryPath
                data.files.append(meta)
                localFileURLs[meta.fileID] = url  // store for FILE_CHUNK_REQUEST serving
            }
            // Hash based on file paths
            let pathString = urls.map { $0.path }.joined(separator: "\n")
            data.contentHash = sha256Hex(Data(pathString.utf8))
            return data
        }

        // Try text
        if let text = pasteboard.string(forType: .string), !text.isEmpty {
            data.contentType = .text
            data.payload = Data(text.utf8)
            data.contentHash = sha256Hex(data.payload)
            return data
        }

        // Try image (PNG)
        if let imageData = pasteboard.data(forType: .png) {
            data.contentType = .image
            data.payload = imageData
            data.contentHash = sha256Hex(imageData)
            return data
        }

        // Try image (TIFF → convert to PNG)
        if let tiffData = pasteboard.data(forType: .tiff),
           let image = NSImage(data: tiffData),
           let pngData = image.pngData() {
            data.contentType = .image
            data.payload = pngData
            data.contentHash = sha256Hex(pngData)
            return data
        }

        return data
    }

    private func writeDataToPasteboard(_ data: Data, type: NSPasteboard.PasteboardType, announcement: Leviathan_ClipboardAnnouncement) {
        switch announcement.contentType {
        case .text:
            if let text = String(data: data, encoding: .utf8) {
                pasteboard.setString(text, forType: type)
            }
        case .image:
            // Try to decode the data into an image for proper format conversion
            if type == .tiff, let image = NSImage(data: data), let tiffData = image.tiffRepresentation {
                pasteboard.setData(tiffData, forType: type)
            } else {
                pasteboard.setData(data, forType: type)
            }
        default:
            pasteboard.setData(data, forType: type)
        }
    }

    private func sha256Hex(_ data: Data) -> String {
        var hash = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        data.withUnsafeBytes { ptr in
            _ = CC_SHA256(ptr.baseAddress, CC_LONG(data.count), &hash)
        }
        return hash.map { String(format: "%02x", $0) }.joined()
    }
}

// MARK: - NSImage Extension

private extension NSImage {
    func pngData() -> Data? {
        guard let tiffRep = tiffRepresentation,
              let bitmap = NSBitmapImageRep(data: tiffRep) else {
            return nil
        }
        return bitmap.representation(using: .png, properties: [:])
    }
}

// MARK: - NSPasteboardItemDataProvider (deferred file rendering)

extension PasteboardManager: NSPasteboardItemDataProvider {

    /// Called by macOS when the user pastes and the file URL data is needed.
    /// Triggers on-demand download from the remote peer, waits for completion,
    /// then provides the local file URL.
    func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem, provideDataForType type: NSPasteboard.PasteboardType) {
        guard type == .fileURL else { return }
        guard let announcement = pendingAnnouncement,
              announcement.contentType == .files else {
            Log.warning("File data provider called but no pending file announcement")
            return
        }

        guard let indexStr = item.string(forType: .init("com.leviathan.clipboard.file-index")),
              let index = Int(indexStr) else {
            Log.error("File data provider: couldn't determine file index")
            return
        }

        Log.info("File data provider: requested file at index \(index)")

        guard let paths = ensureFilesDownloaded(announcement: announcement),
              index < paths.count else {
            Log.error("File data provider: no path for index \(index)")
            return
        }

        let url = URL(fileURLWithPath: paths[index])
        item.setString(url.absoluteString, forType: .fileURL)
        Log.info("Provided file URL: \(paths[index])")
    }

    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {
        Log.info("Pasteboard finished with file data provider")
    }

    /// Coordinates the one-time file download across potentially multiple
    /// concurrent data-provider callbacks (one per file).  The first caller
    /// triggers the download; subsequent callers block until it completes.
    ///
    /// IMPORTANT: The NSPasteboardItemDataProvider callback runs on the main
    /// thread.  A plain semaphore wait would block the main run loop, which
    /// prevents DispatchQueue.main.async blocks (e.g. FileTransferProgress
    /// updates that drive the progress panel) from executing.  Instead we
    /// spin the run loop in small increments so that dispatched UI work —
    /// including the progress panel — can still be processed.
    private func ensureFilesDownloaded(announcement: Leviathan_ClipboardAnnouncement) -> [String]? {
        fileDownloadCondition.lock()

        // Already completed — return cached paths
        if let paths = fileDownloadPaths {
            fileDownloadCondition.unlock()
            return paths
        }

        // First caller triggers the download
        if !fileDownloadTriggered {
            fileDownloadTriggered = true
            fileDownloadCondition.unlock()

            // Send DATA_REQUEST to parent process
            var request = Leviathan_ClipboardDataRequest()
            request.contentHash = announcement.contentHash
            request.contentType = .files
            onDataRequest?(request)

            // Wait for PROVIDE_DATA while keeping the main run loop alive.
            // This allows FileTransferProgress messages (dispatched to main
            // queue by handleMessage) to be processed, so the progress panel
            // can appear and update during the download.
            let deadline = Date(timeIntervalSinceNow: 300)
            var completed = false
            while !completed && Date() < deadline {
                if renderSemaphore.wait(timeout: .now()) == .success {
                    completed = true
                } else if Thread.isMainThread {
                    RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
                } else {
                    Thread.sleep(forTimeInterval: 0.05)
                }
            }

            fileDownloadCondition.lock()
            if completed, let data = renderedData,
               let str = String(data: data, encoding: .utf8) {
                fileDownloadPaths = str.components(separatedBy: "\n").filter { !$0.isEmpty }
                Log.info("File download complete: \(fileDownloadPaths?.count ?? 0) file(s)")
            } else {
                Log.error("File download failed or timed out")
            }
            fileDownloadCondition.broadcast()
            let paths = fileDownloadPaths
            fileDownloadCondition.unlock()
            return paths
        }

        // Another caller while download is in-flight.
        // Poll with run-loop spin to avoid deadlocking when called
        // re-entrantly on the main thread (macOS may fire another
        // data-provider callback while we spin the loop above).
        fileDownloadCondition.unlock()
        let deadline = Date(timeIntervalSinceNow: 300)
        while Date() < deadline {
            fileDownloadCondition.lock()
            let paths = fileDownloadPaths
            fileDownloadCondition.unlock()
            if let paths = paths {
                return paths
            }
            if Thread.isMainThread {
                RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
            } else {
                Thread.sleep(forTimeInterval: 0.05)
            }
        }
        Log.error("Timed out waiting for file download (secondary)")
        return nil
    }
}

