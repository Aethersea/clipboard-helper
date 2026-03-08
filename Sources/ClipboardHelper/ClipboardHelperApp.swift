import AppKit
import ArgumentParser

/// clipboard-helper: A macOS helper process that manages NSPasteboard via Unix domain socket IPC.
///
/// Communicates with a parent process (Go server or Electron/Rust client) using
/// length-prefixed protobuf messages over a Unix socket.
@main
struct ClipboardHelperCommand: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "clipboard-helper",
        abstract: "macOS clipboard helper with NSPasteboard ownership via Unix socket IPC"
    )

    @Option(name: .long, help: "Path to the Unix domain socket")
    var socket: String

    @Option(name: .long, help: "Operating mode: server (Go server) or client (Electron client)")
    var mode: HelperMode = .server

    @Flag(name: .long, help: "Enable verbose debug logging")
    var verbose: Bool = false

    func run() throws {
        Log.verbose = verbose
        Log.info("Starting clipboard-helper (mode: \(mode), socket: \(socket))")

        let app = NSApplication.shared
        let delegate = AppDelegate(socketPath: socket, mode: mode)
        app.delegate = delegate

        // Run the NSApplication event loop — this never returns
        app.run()
    }
}

// MARK: - HelperMode

enum HelperMode: String, ExpressibleByArgument, CustomStringConvertible {
    case server  // Connected to Go server — poll local pasteboard, report changes
    case client  // Connected to Electron client — receive remote content, render lazily

    var description: String { rawValue }
}

// MARK: - AppDelegate

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let socketPath: String
    private let mode: HelperMode
    private var socketServer: SocketServer!
    private var pasteboardManager: PasteboardManager!
    private var fileTransferCoordinator: FileTransferCoordinator!
    private var progressPanel: TransferProgressPanel?

    init(socketPath: String, mode: HelperMode) {
        self.socketPath = socketPath
        self.mode = mode
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Hide from dock (we're a background helper)
        NSApp.setActivationPolicy(.accessory)

        setupComponents()
    }

    func applicationWillTerminate(_ notification: Notification) {
        socketServer?.stop()
        pasteboardManager?.stopPolling()
        Log.info("Shutting down")
    }

    private func setupComponents() {
        // Initialize file transfer coordinator
        fileTransferCoordinator = FileTransferCoordinator()
        fileTransferCoordinator.sendMessage = { [weak self] msg in
            self?.socketServer.send(msg)
        }

        // Initialize pasteboard manager
        pasteboardManager = PasteboardManager()
        // Inject coordinator so it can write NSFilePromiseProviders (client mode)
        pasteboardManager.fileTransferCoordinator = fileTransferCoordinator

        // Initialize socket server
        socketServer = SocketServer(socketPath: socketPath)

        // Wire up callbacks
        pasteboardManager.onClipboardChanged = { [weak self] data in
            self?.handleLocalClipboardChanged(data)
        }

        pasteboardManager.onDataRequest = { [weak self] request in
            self?.handleDataRequest(request)
        }

        socketServer.onMessage = { [weak self] message in
            self?.handleMessage(message)
        }

        socketServer.onClientDisconnected = { [weak self] in
            Log.info("Parent disconnected, stopping pasteboard polling")
            self?.pasteboardManager.stopPolling()
        }

        // Start listening
        do {
            try socketServer.start()
        } catch {
            Log.error("Failed to start socket server: \(error)")
            exit(1)
        }

        // Send READY message once connected
        socketServer.onClientConnected = { [weak self] in
            guard let self = self else { return }
            Log.info("Parent connected")
            self.pasteboardManager.startPolling()
            self.sendReady()
        }

        // Handle SIGTERM / SIGINT for graceful shutdown
        setupSignalHandlers()

        Log.info("clipboard-helper ready (mode: \(mode))")
    }

    // MARK: - Message Handling

    /// Called on the **socket I/O queue** (not main thread).
    /// Operations that touch AppKit / NSPasteboard are dispatched to main.
    /// `provideData` deliberately stays on the socket queue so it can
    /// signal the render semaphore even when the main thread is blocked
    /// inside an NSPasteboardItemDataProvider callback.
    private func handleMessage(_ message: Leviathan_HelperMessage) {
        Log.debug("Received: \(message.type)")

        switch message.type {
        case .setClipboard:
            if case .clipboardData(let data) = message.payload {
                DispatchQueue.main.async { [weak self] in
                    self?.pasteboardManager.setClipboard(data)
                }
            }

        case .announceDelayed:
            if case .announcement(let ann) = message.payload {
                DispatchQueue.main.async { [weak self] in
                    self?.pasteboardManager.announceDelayed(ann)
                }
            }

        case .provideData:
            // MUST NOT dispatch to main — the main thread may be blocked
            // on renderSemaphore inside a data-provider / owner callback.
            // provideData() only sets renderedData and signals the semaphore,
            // which is safe from any thread.
            if case .provideData(let pd) = message.payload {
                pasteboardManager.provideData(pd)
            }

        case .getClipboard:
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                let content = self.pasteboardManager.getCurrentClipboard()
                var response = Leviathan_HelperMessage()
                response.type = .clipboardContent
                response.payload = .clipboardData(content)
                response.timestamp = self.currentTimestamp()
                self.socketServer.send(response)
            }

        case .shutdown:
            Log.info("Shutdown requested")
            socketServer.stop()
            pasteboardManager.stopPolling()
            exit(0)

        case .fileChunkRequest:
            if case .fileChunkRequest(let req) = message.payload {
                handleIncomingFileChunkRequest(req)
            }

        case .fileChunkData:
            if case .fileChunkData(let chunk) = message.payload {
                fileTransferCoordinator.handleChunkData(chunk)
            }

        case .fileTransferProgress:
            if case .fileTransferProgress(let progress) = message.payload {
                DispatchQueue.main.async { [weak self] in
                    self?.handleFileTransferProgress(progress)
                }
            }

        default:
            Log.warning("Unhandled message type: \(message.type)")
        }
    }

    // MARK: - File chunk request (server mode — read local file)

    /// Handle file transfer progress from the Go parent process.
    /// Shows or updates a floating progress panel during file downloads.
    private func handleFileTransferProgress(_ progress: Leviathan_HelperFileTransferProgress) {
        if progress.isComplete {
            Log.info("[FileTransfer] Transfer \(progress.transferID) complete: success=\(progress.success)")
            progressPanel?.completeTransfer(success: progress.success, errorMessage: progress.errorMessage)
            // Auto-dismiss after a short delay
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                self?.progressPanel?.close()
                self?.progressPanel = nil
            }
        } else {
            // Show or update progress
            if progressPanel == nil {
                progressPanel = TransferProgressPanel()
                progressPanel?.showPanel()
            }
            progressPanel?.updateProgress(
                bytesTransferred: progress.bytesTransferred,
                totalBytes: progress.totalBytes
            )
        }
    }

    /// Called when the Go parent needs a chunk of a locally-copied file
    /// (to serve a remote WebRTC client that is pasting from clipboard).
    private func handleIncomingFileChunkRequest(_ req: Leviathan_HelperFileChunkRequest) {
        guard let localURL = pasteboardManager.localFileURLs[req.fileID] else {
            Log.warning("[FileTransfer] FILE_CHUNK_REQUEST for unknown fileID=\(req.fileID)")
            sendFileChunkError(transferID: req.transferID, fileID: req.fileID,
                               offset: req.offset, error: "file not found")
            return
        }

        let chunkSize = req.size > 0 ? Int(req.size) : 262_144
        let offset = req.offset

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            do {
                let fh = try FileHandle(forReadingFrom: localURL)
                defer { fh.closeFile() }

                let fileSize = (try? FileManager.default.attributesOfItem(atPath: localURL.path))?[.size] as? UInt64 ?? 0

                if offset >= fileSize && fileSize > 0 {
                    // Past EOF — send empty last chunk
                    self.sendFileChunk(
                        transferID: req.transferID, fileID: req.fileID,
                        offset: offset, data: Data(), isLast: true)
                    return
                }

                try fh.seek(toOffset: offset)
                let toRead = min(chunkSize, Int(fileSize - offset))
                let data = fh.readData(ofLength: toRead)
                let isLast = (offset + UInt64(data.count)) >= fileSize

                self.sendFileChunk(
                    transferID: req.transferID, fileID: req.fileID,
                    offset: offset, data: data, isLast: isLast)

                Log.debug("[FileTransfer] Served chunk offset=\(offset) size=\(data.count) isLast=\(isLast)")
            } catch {
                self.sendFileChunkError(
                    transferID: req.transferID, fileID: req.fileID,
                    offset: offset, error: error.localizedDescription)
            }
        }
    }

    private func sendFileChunk(transferID: String, fileID: String,
                                offset: UInt64, data: Data, isLast: Bool) {
        var chunk = Leviathan_HelperFileChunkData()
        chunk.transferID = transferID
        chunk.fileID = fileID
        chunk.offset = offset
        chunk.data = data
        chunk.isLast = isLast

        var msg = Leviathan_HelperMessage()
        msg.type = .fileChunkData
        msg.payload = .fileChunkData(chunk)
        msg.timestamp = currentTimestamp()
        socketServer.send(msg)
    }

    private func sendFileChunkError(transferID: String, fileID: String,
                                     offset: UInt64, error: String) {
        var chunk = Leviathan_HelperFileChunkData()
        chunk.transferID = transferID
        chunk.fileID = fileID
        chunk.offset = offset
        chunk.error = error
        chunk.isLast = true

        var msg = Leviathan_HelperMessage()
        msg.type = .fileChunkData
        msg.payload = .fileChunkData(chunk)
        msg.timestamp = currentTimestamp()
        socketServer.send(msg)
    }

    // MARK: - Outgoing Messages

    private func handleLocalClipboardChanged(_ data: Leviathan_ClipboardData) {
        var message = Leviathan_HelperMessage()
        message.type = .clipboardChanged
        message.payload = .clipboardData(data)
        message.timestamp = currentTimestamp()
        socketServer.send(message)
    }

    private func handleDataRequest(_ request: Leviathan_ClipboardDataRequest) {
        var message = Leviathan_HelperMessage()
        message.type = .dataRequest
        message.payload = .dataRequest(request)
        message.timestamp = currentTimestamp()
        socketServer.send(message)
    }

    private func sendReady() {
        var message = Leviathan_HelperMessage()
        message.type = .ready
        message.timestamp = currentTimestamp()
        socketServer.send(message)
    }

    private func sendError(_ errorMsg: String) {
        var message = Leviathan_HelperMessage()
        message.type = .error
        message.payload = .errorMessage(errorMsg)
        message.timestamp = currentTimestamp()
        socketServer.send(message)
    }

    // MARK: - Utilities

    private func currentTimestamp() -> UInt64 {
        UInt64(Date().timeIntervalSince1970 * 1000)
    }

    private func setupSignalHandlers() {
        let signalSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        signal(SIGTERM, SIG_IGN)
        signalSource.setEventHandler { [weak self] in
            Log.info("SIGTERM received")
            self?.socketServer.stop()
            self?.pasteboardManager.stopPolling()
            exit(0)
        }
        signalSource.resume()

        let intSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        signal(SIGINT, SIG_IGN)
        intSource.setEventHandler { [weak self] in
            Log.info("SIGINT received")
            self?.socketServer.stop()
            self?.pasteboardManager.stopPolling()
            exit(0)
        }
        intSource.resume()
    }
}
