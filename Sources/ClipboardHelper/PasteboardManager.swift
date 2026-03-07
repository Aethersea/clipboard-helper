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

    // Callbacks
    var onClipboardChanged: ((Leviathan_ClipboardData) -> Void)?
    var onDataRequest: ((Leviathan_ClipboardDataRequest) -> Void)?

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
            // Write file URLs if file metadata is provided
            var urls: [NSURL] = []
            for file in data.files {
                if !file.filename.isEmpty {
                    // File URLs would be resolved by the parent process
                    Log.info("Set clipboard: file \(file.filename)")
                }
            }
            if !urls.isEmpty {
                pasteboard.writeObjects(urls)
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
            types = [.fileURL]
        default:
            break
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
            Log.error("Timed out waiting for data from parent")
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

        // Try text first
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

        // Try file URLs
        if let urls = pasteboard.readObjects(forClasses: [NSURL.self], options: [
            .urlReadingFileURLsOnly: true
        ]) as? [URL], !urls.isEmpty {
            data.contentType = .files
            for (i, url) in urls.enumerated() {
                var meta = Leviathan_FileMetadata()
                meta.fileID = "\(i)"
                meta.filename = url.lastPathComponent
                meta.relativePath = url.lastPathComponent
                if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path) {
                    meta.fileSize = (attrs[.size] as? UInt64) ?? 0
                }
                meta.isDirectory = url.hasDirectoryPath
                data.files.append(meta)
            }
            // Hash based on file paths
            let pathString = urls.map { $0.path }.joined(separator: "\n")
            data.contentHash = sha256Hex(Data(pathString.utf8))
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
