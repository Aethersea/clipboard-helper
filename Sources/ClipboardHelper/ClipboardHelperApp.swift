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
        // Initialize pasteboard manager
        pasteboardManager = PasteboardManager()

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

        socketServer.onClientConnected = { [weak self] in
            Log.info("Parent connected, starting pasteboard polling")
            self?.pasteboardManager.startPolling()
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

    private func handleMessage(_ message: Leviathan_HelperMessage) {
        Log.debug("Received: \(message.type)")

        switch message.type {
        case .setClipboard:
            if case .clipboardData(let data) = message.payload {
                pasteboardManager.setClipboard(data)
            }

        case .announceDelayed:
            if case .announcement(let ann) = message.payload {
                pasteboardManager.announceDelayed(ann)
            }

        case .provideData:
            if case .provideData(let pd) = message.payload {
                pasteboardManager.provideData(pd)
            }

        case .getClipboard:
            let content = pasteboardManager.getCurrentClipboard()
            var response = Leviathan_HelperMessage()
            response.type = .clipboardContent
            response.payload = .clipboardData(content)
            response.timestamp = currentTimestamp()
            socketServer.send(response)

        case .shutdown:
            Log.info("Shutdown requested")
            socketServer.stop()
            pasteboardManager.stopPolling()
            exit(0)

        default:
            Log.warning("Unhandled message type: \(message.type)")
        }
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
