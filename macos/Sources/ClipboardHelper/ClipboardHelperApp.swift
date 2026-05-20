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

    // Int (not Int32) for ExpressibleByArgument coverage across older
    // swift-argument-parser versions — narrowed to pid_t at the call site.
    @Option(
        name: .long,
        help: "Parent process ID. When supplied the helper watches this PID and exits if it dies, mirroring the Windows --parent-pid watchdog and preventing orphan helpers from accumulating after parent crashes / force-quits."
    )
    var parentPid: Int?

    @Flag(name: .long, help: "Enable verbose debug logging")
    var verbose: Bool = false

    func run() throws {
        Log.verbose = verbose
        Log.info("Starting clipboard-helper (mode: \(mode), socket: \(socket))")

        let app = NSApplication.shared
        let delegate = AppDelegate(
            socketPath: socket,
            mode: mode,
            parentPid: parentPid.map { pid_t($0) }
        )
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
    private let parentPid: pid_t?
    private var socketServer: SocketServer!
    private var pasteboardManager: PasteboardManager!
    private var fileTransferCoordinator: FileTransferCoordinator!
    private var progressPanel: TransferProgressPanel?
    /// Transfer ID the current panel is showing.  Used by
    /// ensureProgressPanelVisible to detect a transferID transition — if a
    /// new transfer starts while the previous panel is still on screen
    /// (e.g. inside the 1.5s post-complete dismiss window), we close the
    /// stale panel and create a fresh one so the user doesn't see leftover
    /// "Transfer complete" chrome.
    private var activeTransferID: String?
    /// Pending main-queue work item for the 1.5s panel auto-dismiss.
    /// Held so we can cancel it if a new transfer starts before it fires —
    /// otherwise the stale timer would close the new panel out from under
    /// the new transfer.
    private var pendingPanelCloseWorkItem: DispatchWorkItem?
    /// Retained for lifetime of the helper — cancelling the source unregisters
    /// the kqueue watch, so it must outlive setupParentPidWatchdog's scope.
    private var parentPidWatcher: DispatchSourceProcess?

    init(socketPath: String, mode: HelperMode, parentPid: pid_t?) {
        self.socketPath = socketPath
        self.mode = mode
        self.parentPid = parentPid
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Hide from dock (we're a background helper)
        NSApp.setActivationPolicy(.accessory)

        setupComponents()
        setupParentPidWatchdog()
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

        // Upstream cancel notification.  Encoded as a HELPER_MESSAGE_TYPE
        // _ERROR with a structured prefix that shen-mac native parses
        // (see clipboard/macos.rs handler for HelperMessageType::Error)
        // so we avoid adding a new HelperMessage variant + the matching
        // proto regen across three repos.
        pasteboardManager.onCancelTransfer = { [weak self] transferID in
            guard let self = self else { return }
            var msg = Leviathan_HelperMessage()
            msg.type = .error
            msg.errorMessage = "FILE_TRANSFER_CANCEL:\(transferID)"
            msg.timestamp = UInt64(Date().timeIntervalSince1970 * 1000)
            self.socketServer.send(msg)
            Log.info("Sent upstream FILE_TRANSFER_CANCEL for transfer \(transferID)")
        }

        // User pressed Cmd+V — pop the panel immediately in
        // indeterminate state so the user always has visible feedback,
        // even when the transfer completes before any intermediate
        // progress frame arrives.
        pasteboardManager.onTransferStart = { [weak self] transferID in
            guard let self = self else { return }
            self.ensureProgressPanelVisible(for: transferID)
        }

        // KVO-driven progress from the NSFilePromiseProvider path
        // (FilePromiseHandler.writePromiseTo observes
        // parentProgress.fractionCompleted).  Finder's actual paste path
        // doesn't emit IPC FILE_TRANSFER_PROGRESS frames, so this
        // callback is what keeps the panel updated when the user pastes
        // into Finder.
        pasteboardManager.onTransferProgress = { [weak self] transferID, transferred, total in
            guard let self = self else { return }
            // Defensive ensure — KVO observation can fire before
            // onTransferStart in rare orderings on the operation queue.
            self.ensureProgressPanelVisible(for: transferID)
            self.progressPanel?.updateProgress(
                bytesTransferred: transferred,
                totalBytes: total
            )
        }

        // KVO-driven completion for the NSFilePromiseProvider path.
        // Mirrors the auto-dismiss timing used by handleFileTransferProgress
        // so the UX is uniform across both pipelines.
        pasteboardManager.onTransferComplete = { [weak self] _, success, errorMessage in
            guard let self = self else { return }
            Log.info("[FileTransfer] Promise transfer complete: success=\(success)")
            self.progressPanel?.completeTransfer(success: success, errorMessage: errorMessage)
            self.scheduleProgressPanelDismiss()
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
        // Always ensure the panel exists — even on a terminal isComplete
        // frame.  Without this, a small/fast transfer whose only
        // progress frame is `isComplete=true` would never get a panel
        // (completeTransfer is a no-op on a nil panel) and the user
        // would see nothing despite the paste happening.
        ensureProgressPanelVisible(for: progress.transferID)

        if progress.isComplete {
            Log.info("[FileTransfer] Transfer \(progress.transferID) complete: success=\(progress.success)")
            progressPanel?.completeTransfer(success: progress.success, errorMessage: progress.errorMessage)
            scheduleProgressPanelDismiss()
        } else {
            progressPanel?.updateProgress(
                bytesTransferred: progress.bytesTransferred,
                totalBytes: progress.totalBytes
            )
        }
    }

    /// Create + show the progress panel if it doesn't already exist.
    /// Wires the Cancel button to PasteboardManager.cancelFileDownload
    /// with the given transferID so late cancels for stale transfers
    /// get filtered out at the PasteboardManager layer.
    ///
    /// Called from two paths:
    ///   * `pasteboardManager.onTransferStart` — immediately when the
    ///     user pastes, before any progress frame has been received.
    ///     Panel comes up in indeterminate state.
    ///   * `handleFileTransferProgress` — defensive recreate, in case
    ///     the terminal isComplete frame arrives before any intermediate
    ///     progress frame (small/fast transfers).
    ///
    /// If a panel already exists but for a DIFFERENT transferID, we tear
    /// it down and rebuild — this covers the window where a new paste
    /// starts during the 1.5s post-complete auto-dismiss delay (the
    /// previous panel would otherwise be stuck on "Transfer complete"
    /// with a disabled Cancel button).  Any pending dismiss timer is
    /// always cancelled so it can't close the new panel later.
    ///
    /// Same-transferID re-entry: NSFilePromiseProvider invokes
    /// `writePromiseTo` once per file, and every file in a multi-file
    /// paste shares the same announcement-scoped transferID (it's
    /// `pending.content_hash`).  If file N just completed (panel now in
    /// "Transfer complete" terminal state + dismiss timer pending) and
    /// file N+1 starts within 1.5 s, we cancel the timer and reset the
    /// panel's UI back to running so the user doesn't see a stale
    /// terminal panel while file N+1 silently downloads.  Detecting the
    /// pending timer (vs. always resetting) keeps the very-first
    /// ensureProgressPanelVisible-of-a-fresh-paste cheap.
    private func ensureProgressPanelVisible(for transferID: String) {
        let hadPendingDismiss = pendingPanelCloseWorkItem != nil
        pendingPanelCloseWorkItem?.cancel()
        pendingPanelCloseWorkItem = nil

        if progressPanel != nil && activeTransferID != transferID {
            progressPanel?.close()
            progressPanel = nil
        }

        if progressPanel == nil {
            progressPanel = TransferProgressPanel()
            progressPanel?.onCancel = { [weak self] in
                self?.pasteboardManager.cancelFileDownload(transferID: transferID)
            }
            progressPanel?.showPanel()
        } else if hadPendingDismiss {
            progressPanel?.resetForNextTransfer()
        }
        activeTransferID = transferID
    }

    /// Mark the current panel as terminal (success or failure), then
    /// schedule auto-dismiss in 1.5 s.  The scheduled work item is held
    /// in `pendingPanelCloseWorkItem` so a fresh transfer arriving
    /// inside the window can cancel it via ensureProgressPanelVisible.
    /// Capturing `panel` weakly inside the timer ensures we only null
    /// out the field when we close the SAME panel we scheduled against.
    private func scheduleProgressPanelDismiss() {
        pendingPanelCloseWorkItem?.cancel()
        let scheduledPanel = progressPanel
        let workItem = DispatchWorkItem { [weak self, weak scheduledPanel] in
            guard let self = self else { return }
            scheduledPanel?.close()
            if self.progressPanel === scheduledPanel {
                self.progressPanel = nil
                self.activeTransferID = nil
            }
            self.pendingPanelCloseWorkItem = nil
        }
        pendingPanelCloseWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: workItem)
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

                // Past EOF (or empty file at offset 0): send a terminal
                // empty chunk so the client's pipeline can complete its
                // accounting and tear the transfer down.  The previous
                // `&& fileSize > 0` guard misrouted 0-byte files into
                // the seek/read path, where `min(chunkSize, fileSize -
                // offset)` becomes 0 and an empty read still works, but
                // it relies on UInt64 underflow not happening (offset 0
                // - fileSize 0 is fine, larger offsets aren't) — the
                // explicit branch is safer.
                if offset >= fileSize {
                    self.sendFileChunk(
                        transferID: req.transferID, fileID: req.fileID,
                        offset: offset, data: Data(), isLast: true)
                    return
                }

                try fh.seek(toOffset: offset)
                // Order the min as `min(UInt64, UInt64)` *before*
                // narrowing to Int so that an absurdly large `fileSize -
                // offset` (e.g. on theoretical >Int.max files) can't
                // trap on the Int() conversion.  In practice APFS caps
                // at 8 EB which fits in Int.max (9 EB), but the cheaper
                // form is also the safer one.
                let remaining = fileSize - offset
                let toRead = Int(min(UInt64(chunkSize), remaining))
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

    /// Mirror the Windows helper's parent-pid watchdog: if the parent process
    /// (shen / leviathan) dies without sending SHUTDOWN — crash, SIGKILL,
    /// macOS Force Quit, dev-time HMR reload — we'd otherwise survive as a
    /// system-background orphan and accumulate across restarts. DispatchSource
    /// .makeProcessSource hooks the kqueue NOTE_EXIT filter on Darwin, so the
    /// kernel signals us the instant the parent disappears.
    private func setupParentPidWatchdog() {
        guard let pid = parentPid, pid > 0 else { return }

        // Install the kqueue source FIRST, then race-check. The check has to
        // happen after install so we can't miss an exit that lands in the gap
        // between the check and the install. getppid() is the definitive
        // signal on Darwin: when a parent dies its children are reparented to
        // launchd (pid 1), so getppid() != parentPid means parent is gone.
        let source = DispatchSource.makeProcessSource(
            identifier: pid,
            eventMask: .exit,
            queue: .main
        )
        source.setEventHandler { [weak self] in
            Log.info("Parent pid \(pid) exited, shutting down")
            self?.socketServer?.stop()
            self?.pasteboardManager?.stopPolling()
            exit(0)
        }
        source.resume()
        parentPidWatcher = source

        let actualPpid = getppid()
        if actualPpid != pid {
            Log.info(
                "Parent pid \(pid) already reparented to \(actualPpid) before watchdog armed — exiting"
            )
            exit(0)
        }

        Log.info("Watching parent pid \(pid) — will exit when parent dies")
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
