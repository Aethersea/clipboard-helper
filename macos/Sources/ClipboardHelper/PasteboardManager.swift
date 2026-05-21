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

    // Delayed rendering state.
    //
    // `pendingAnnouncement`, `renderedData`, `renderedTiff`, and
    // `isRequestInFlight` are read on the main thread (inside
    // provideDataForType:) and mutated on the socket I/O queue (inside
    // provideData when PROVIDE_DATA arrives). All accesses go through
    // `stateLock` to avoid torn reads of the Optional<Data> values and to
    // make the "dedupe concurrent requests" check race-free.
    private let stateLock = NSLock()
    private var pendingAnnouncement: Leviathan_ClipboardAnnouncement?
    private var renderSemaphore = DispatchSemaphore(value: 0)
    private var renderedData: Data?
    // Pre-rendered .tiff representation, computed on the socket queue when
    // PROVIDE_DATA arrives so that pasteboard.data(forType: .tiff) doesn't
    // spend seconds decoding+re-encoding the image on the main thread.
    // Set alongside `renderedData`; nil if conversion failed or the
    // announcement isn't an image.
    private var renderedTiff: Data?
    // True between the moment we send a DATA_REQUEST for the current
    // pending announcement and the moment we receive a matching
    // PROVIDE_DATA (or the announcement is replaced/cleared). When set,
    // subsequent provideDataForType: callbacks for OTHER types of the
    // same announcement (e.g. PNG followed by TIFF in apps that probe
    // both) skip sending a duplicate DATA_REQUEST and just wait on the
    // same semaphore.
    private var isRequestInFlight = false
    private var lastSetHash: String?

    /// Wall-clock time of the last announceDelayedFiles publication.
    /// Used purely as a diagnostic in `pasteboard:provideDataForType:`
    /// logging so we can distinguish "system probe < 1 s after announce"
    /// (Universal Clipboard, indexers) from "real user Cmd+V" (typically
    /// seconds-to-minutes later).
    private var lastAnnounceTime: Date?

    // File transfer coordinator (injected by AppDelegate in client mode)
    var fileTransferCoordinator: FileTransferCoordinator?

    // Local file URLs from the last pasteboard read, keyed by file_id.
    // Used in server mode so AppDelegate can serve FILE_CHUNK_REQUESTs.
    private(set) var localFileURLs: [String: URL] = [:]

    // Callbacks
    var onClipboardChanged: ((Leviathan_ClipboardData) -> Void)?
    var onDataRequest: ((Leviathan_ClipboardDataRequest) -> Void)?
    /// Fired when the user clicks Cancel on the TransferProgressPanel.
    /// Wired by ClipboardHelperApp to send a HELPER_MESSAGE_TYPE_ERROR
    /// with body `FILE_TRANSFER_CANCEL:<transferID>` upstream, which
    /// shen-mac native parses and propagates to leviathan as a
    /// WebRTC ClipboardMessageType::FileTransferCancel.
    var onCancelTransfer: ((String) -> Void)?

    /// Fired the moment the user actually pastes (either via Finder's
    /// NSFilePromiseProvider writePromiseTo path or via the legacy
    /// `.fileURL` data-provider path) — NOT on announcement.  Wired by
    /// ClipboardHelperApp to pop up the TransferProgressPanel in
    /// indeterminate state so the user always has immediate feedback.
    var onTransferStart: ((String) -> Void)?

    /// Fired per byte-progress update for the NSFilePromiseProvider
    /// path.  Driven by KVO on the parentProgress object inside
    /// FilePromiseHandler.writePromiseTo — the `.fileURL` legacy path
    /// already drives the panel via IPC FILE_TRANSFER_PROGRESS messages
    /// so it doesn't need this callback.
    var onTransferProgress: ((_ transferID: String,
                              _ bytesTransferred: UInt64,
                              _ totalBytes: UInt64) -> Void)?

    /// Fired when an NSFilePromiseProvider transfer finishes (success
    /// or failure).  Closes the panel.  The `.fileURL` legacy path
    /// signals completion through the IPC FILE_TRANSFER_PROGRESS frame
    /// with `is_complete=true`.
    var onTransferComplete: ((_ transferID: String,
                              _ success: Bool,
                              _ errorMessage: String) -> Void)?

    // File download coordination (for NSPasteboardItemDataProvider)
    private var fileDownloadPaths: [String]?
    private var fileDownloadTriggered = false
    private var fileDownloadCancelled = false
    private let fileDownloadCondition = NSCondition()

    // Phase 2 (NSFilePromiseProvider dual-publish, see plan-mac-virtual-file-
    // clipboard.md).
    //   * activePromiseProviders pins the per-announcement
    //     NSFilePromiseProvider instances so they survive long enough for
    //     `writePromiseTo:` to fire — AppKit holds only a weak reference
    //     through the pasteboard, so without us retaining, an active
    //     download could lose its delegate mid-flight when the autorelease
    //     pool drains.
    //   * activeTransferID is the announcement's transfer_id captured at
    //     publish time. cancelTransfer(_:) on FileTransferCoordinator uses
    //     it when a fresh announcement supersedes the current one.
    // Mutated only on the main thread (inside announceDelayedFiles +
    // pasteboardChangedOwner), so no lock is needed.
    fileprivate var activePromiseProviders: [NSFilePromiseProvider] = []
    fileprivate var activeTransferID: String?

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

        // Tear down any Files dual-publish state from a prior
        // ANNOUNCE_DELAYED — SetClipboard overrides any pending lazy
        // rendering, so in-flight FileTransferCoordinator downloads
        // are now writing to a destination the OS no longer cares
        // about. Cancel them to free FILE_CHUNK_REQUEST bandwidth and
        // close Finder's progress dialog (if any) cleanly.
        if let prevTID = activeTransferID {
            fileTransferCoordinator?.cancelTransfer(prevTID)
        }
        activePromiseProviders.removeAll()
        activeTransferID = nil

        // `prepareForNewContents(with: .currentHostOnly)` replaces
        // `clearContents()` and explicitly opts the new pasteboard
        // contents out of Universal Clipboard / Continuity / Handoff
        // sync.  Apple's documented mechanism to suppress `sharingd`'s
        // synchronous probe right after writeObjects.
        //
        // Trade-off: the user can't paste this content on their iPhone
        // / other Macs.  Correct for our case — the content is on a
        // remote server and the `.fileURL` we publish points to a
        // local cache directory that doesn't exist on other devices,
        // so a Universal Clipboard "paste" elsewhere would be useless
        // anyway.
        pasteboard.prepareForNewContents(with: .currentHostOnly)

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

        // Tear down any prior Files dual-publish state regardless of the
        // new content type. Without this, a Files announcement followed
        // by a Text announcement would leave the previous file transfer's
        // chunks in flight against a dead destination URL, and the prior
        // `activePromiseProviders` would linger uselessly in memory.
        // announceDelayedFiles re-establishes both fields below if the
        // new type is Files; otherwise they stay cleared.
        if let prevTID = activeTransferID {
            fileTransferCoordinator?.cancelTransfer(prevTID)
        }
        activePromiseProviders.removeAll()
        activeTransferID = nil

        stateLock.lock()
        pendingAnnouncement = announcement
        lastSetHash = announcement.contentHash
        renderedData = nil
        renderedTiff = nil
        isRequestInFlight = false
        stateLock.unlock()

        // Drain any stale semaphore signals from a previous announcement whose
        // PROVIDE_DATA arrived after we timed out. Without this, the next
        // provideDataForType returns immediately with renderedData=nil and the
        // paste silently produces nothing.
        while renderSemaphore.wait(timeout: .now()) == .success {}

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

        // Outer `announceDelayed` already cancelled any prior transfer
        // and cleared activePromiseProviders / activeTransferID; we just
        // need to re-establish them for the new Files announcement.
        activeTransferID = announcement.transferID

        // Register the new announcement with the coordinator so
        // FILE_CHUNK_REQUEST dispatch can resolve (transfer_id, file_id) →
        // metadata. Without this the promise path's startFileDownload
        // would have nothing to look up in fileMetadata(transferID:fileID:).
        fileTransferCoordinator?.registerAnnouncement(announcement)

        // Store announcement so the data provider callback can reference it
        stateLock.lock()
        pendingAnnouncement = announcement
        lastSetHash = announcement.contentHash
        renderedData = nil
        renderedTiff = nil
        isRequestInFlight = false
        stateLock.unlock()

        // Reset file download state
        fileDownloadCondition.lock()
        fileDownloadPaths = nil
        fileDownloadTriggered = false
        fileDownloadCancelled = false
        fileDownloadCondition.unlock()

        // Drain any stale semaphore signals from previous operations
        while renderSemaphore.wait(timeout: .now()) == .success {}

        // Dual-publish: NSFilePromiseProvider + NSPasteboardItem with
        // `.fileURL` data provider per file.  Both are required:
        //
        //   * NSFilePromiseProvider covers drag-and-drop (Finder /
        //     Mail / Notes drag targets) — Apple's official "Supporting
        //     Drag and Drop Through File Promises" sample is built
        //     around this API and assumes the dragging pasteboard.
        //   * NSPasteboardItem with `.fileURL` covers Cmd+C / Cmd+V on
        //     NSPasteboard.general — Finder's Paste menu enables only
        //     when it sees `.fileURL` (NSFilePromiseProvider written to
        //     .general is invisible to Finder's clipboard-paste path
        //     by Apple's design split between dragging and copy-paste
        //     pasteboards; confirmed by both Apple docs and empirical
        //     testing — removing this half disables the Paste menu).
        //
        // The `.fileURL` data provider is synchronous on main thread;
        // bytes are still fetched lazily (per-chunk via
        // ensureFilesDownloaded → DATA_REQUEST chain that drives
        // FileTransferCoordinator) without blocking Finder thanks to
        // the 50 ms run-loop spin in ensureFilesDownloaded.
        // Ordering matters: the NSPasteboardItem(.fileURL) entries are
        // appended BEFORE their corresponding NSFilePromiseProvider so
        // that the first item on the pasteboard carries both:
        //   1. `.fileURL` — required for Finder's Paste menu / other
        //      consumers that use `availableType(from:)`, which only
        //      inspects the first item's types.
        //   2. The http://nspasteboard.org/ markers (below) — third-
        //      party clipboard managers (Alfred / Raycast / Maccy /
        //      Paste / …) read `pasteboard.types` first and skip
        //      pasteboards whose first item is tagged transient.
        //      Without skipping, those tools call
        //      readObjects(forClasses: [NSURL.self]) which synchronously
        //      fires the `.fileURL` data provider, kicking off a remote
        //      download at announce time — hours before the user actually
        //      pastes.  Every `.fileURL` item is tagged so managers that
        //      iterate items rather than just checking item 0 also skip.
        // NSFilePromiseProvider items remain on the pasteboard for
        // drag-and-drop targets (which Apple's docs say use the dragging
        // pasteboard for promises) and are NOT probe-able via
        // readObjects(NSURL) — their bytes only materialise through
        // writePromiseTo — so they don't need the markers.
        let fileIndexType = NSPasteboard.PasteboardType("com.leviathan.clipboard.file-index")
        let transientType = NSPasteboard.PasteboardType("org.nspasteboard.TransientType")
        let concealedType = NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType")
        let autoGenType   = NSPasteboard.PasteboardType("org.nspasteboard.AutoGeneratedType")
        // Source identifier per http://nspasteboard.org/ — declares which
        // "app" produced this clipboard item.  Maccy / Paste / Raycast et al.
        // read this type and use it as the source for their "ignore by
        // source" filter (the helper has no Info.plist → no real Bundle
        // ID, so without this string they'd fall back to whatever app
        // was frontmost at write time, typically the parent Electron
        // process, which the user can't bypass without blanket-blocking
        // all Electron-originated copies).
        let sourceType   = NSPasteboard.PasteboardType("org.nspasteboard.source")
        let emptyTagData = Data()
        let sourceIdentifier = "com.aethersea.clipboard-helper"
        let sourceIdentifierData = Data(sourceIdentifier.utf8)

        var writings: [NSPasteboardWriting] = []
        for (i, file) in announcement.files.enumerated() {
            let item = NSPasteboardItem()
            item.setString("\(i)", forType: fileIndexType)
            item.setDataProvider(self, forTypes: [.fileURL])
            item.setData(emptyTagData, forType: transientType)
            item.setData(emptyTagData, forType: concealedType)
            item.setData(emptyTagData, forType: autoGenType)
            item.setData(sourceIdentifierData, forType: sourceType)
            writings.append(item)

            let provider = makeFilePromiseProvider(for: file,
                                                   transferID: announcement.transferID)
            activePromiseProviders.append(provider)
            writings.append(provider)
        }

        // See `setClipboard`'s identical call for the rationale.  The
        // `.currentHostOnly` opt-out is what blocks Continuity /
        // Universal Clipboard / sharingd from synchronously probing
        // our `.fileURL` data provider milliseconds after writeObjects
        // — Apple's documented suppression mechanism (10.13+).
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.writeObjects(writings)
        lastChangeCount = pasteboard.changeCount
        lastAnnounceTime = Date()

        // No eager pre-download.  An earlier version dispatched
        // ensureFilesDownloaded here so the `.fileURL` data provider
        // could return from cache, but that triggers a full dcTransfer
        // download at announcement time — even when the user never
        // pastes — and also pops the TransferProgressPanel via the
        // resulting IPC FILE_TRANSFER_PROGRESS frames, which is the
        // wrong UX (the user hasn't pasted yet).  Both pipelines now
        // start the download lazily when the user actually pastes:
        //
        //   * Finder (NSFilePromiseProvider path): writePromiseTo →
        //     FileTransferCoordinator chunked download via
        //     FILE_CHUNK_REQUEST, progress surfaced through KVO on
        //     NSProgress to the helper's TransferProgressPanel.
        //   * Other targets (`.fileURL` path): pasteboard:provideDataForType:
        //     synchronously drives ensureFilesDownloaded on first paste,
        //     blocking the main run loop spin until the download
        //     completes — which is the price for not wasting bandwidth.

        Log.info("Announced delayed files (dual-publish): \(announcement.files.count) file(s) hash=\(announcement.contentHash) transferID=\(announcement.transferID)")
    }

    // MARK: - Provide Data (response from parent)

    /// Called on the socket I/O queue when PROVIDE_DATA arrives. Mutates
    /// `renderedData` / `renderedTiff` / `isRequestInFlight` under
    /// `stateLock` so the main-thread provideDataForType: reader sees a
    /// consistent view, then signals the render semaphore.
    func provideData(_ data: Leviathan_HelperProvideData) {
        stateLock.lock()
        // Drop data that doesn't match the current pending announcement.
        // Otherwise a late response from a previous request would either
        // wedge a stale signal in the semaphore or clobber the next paste
        // with the wrong content.
        guard let pending = pendingAnnouncement,
              shouldAcceptProvideData(pending: pending,
                                      incomingHash: data.contentHash) else {
            let pendingHash = pendingAnnouncement?.contentHash ?? "<none>"
            stateLock.unlock()
            Log.warning("Discarding PROVIDE_DATA: hash=\(data.contentHash) does not match pending=\(pendingHash)")
            return
        }

        renderedData = data.data
        isRequestInFlight = false

        // We're on the socket I/O queue here (background). Pre-render the
        // .tiff representation now so the main thread doesn't pay the
        // NSImage decode + tiffRepresentation cost inside the blocking
        // provideDataForType: callback for apps that prefer .tiff (Preview,
        // Pages, etc.). For an 8K screenshot this conversion is ~1–3 s on
        // main thread, which directly translates to paste-target freeze.
        var tiff: Data? = nil
        if pending.contentType == .image && !data.data.isEmpty {
            if let image = NSImage(data: data.data),
               let tiffRep = image.tiffRepresentation {
                tiff = tiffRep
                Log.info("Pre-rendered .tiff representation: \(tiffRep.count) bytes")
            } else {
                Log.warning("Failed to pre-render .tiff; will fall back to on-demand conversion")
            }
        }
        renderedTiff = tiff
        stateLock.unlock()

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
        // Snapshot the announcement + any already-cached bytes under the
        // lock; mutate isRequestInFlight only if we're the first caller
        // for this paste burst.
        stateLock.lock()
        guard let announcement = pendingAnnouncement else {
            stateLock.unlock()
            Log.warning("provideDataForType called but no pending announcement")
            return
        }

        // Early-return for types we never declared. macOS' pasteboard
        // server can probe an owner for types beyond what was passed to
        // declareTypes (.pdf, .rtf, type-converted variants). Without
        // this guard we would still run the full DATA_REQUEST round-trip
        // + 10 s wait for those probes even though the response is empty
        // and the pasteboard call would have failed cleanly otherwise.
        let allowedTypes: [NSPasteboard.PasteboardType]
        switch announcement.contentType {
        case .text:
            allowedTypes = [.string]
        case .image:
            allowedTypes = [.png, .tiff]
        default:
            allowedTypes = []
        }
        guard allowedTypes.contains(type) else {
            stateLock.unlock()
            Log.info("provideDataForType: ignoring undeclared type \(type.rawValue)")
            return
        }

        // If we already have rendered data (cached from a previous format
        // request for the same paste), provide it directly.
        if let cachedData = renderedData {
            let cachedTiff = renderedTiff
            stateLock.unlock()
            writeDataToPasteboard(cachedData,
                                  cachedTiff: cachedTiff,
                                  type: type,
                                  announcement: announcement)
            return
        }

        // Dedupe concurrent format probes. If apps query both .png and
        // .tiff in quick succession, only the first should fire a
        // DATA_REQUEST; the second just waits on the same semaphore.
        let shouldSendRequest = !isRequestInFlight
        if shouldSendRequest {
            isRequestInFlight = true
        }
        stateLock.unlock()

        if shouldSendRequest {
            Log.info("OS requested data for type: \(type.rawValue) (sending DATA_REQUEST)")
            var request = Leviathan_ClipboardDataRequest()
            request.contentHash = announcement.contentHash
            request.contentType = announcement.contentType
            onDataRequest?(request)
        } else {
            Log.info("OS requested data for type: \(type.rawValue) (request already in flight)")
        }

        // Block waiting for PROVIDE_DATA. Capped at 10 s — long enough for
        // a typical WebRTC round-trip plus margin, short enough that a
        // failed fetch presents as a brief beach-ball rather than a
        // permanent freeze on the paste-target app's main thread. Matched
        // by shen's handle_data_request_data deadline.
        let result = renderSemaphore.wait(timeout: .now() + 10)

        if result == .timedOut {
            Log.error("Timed out waiting for data from parent (type=\(announcement.contentType))")
            return
        }

        stateLock.lock()
        let dataCopy = renderedData
        let tiffCopy = renderedTiff
        stateLock.unlock()

        guard let data = dataCopy else {
            Log.error("No data received after signal")
            return
        }

        writeDataToPasteboard(data,
                              cachedTiff: tiffCopy,
                              type: type,
                              announcement: announcement)
    }

    @objc func pasteboardChangedOwner(_ sender: NSPasteboard) {
        Log.info("Pasteboard ownership lost")
        stateLock.lock()
        pendingAnnouncement = nil
        renderedData = nil
        renderedTiff = nil
        isRequestInFlight = false
        stateLock.unlock()
        // Drop any residual signal so the next announcement starts clean.
        while renderSemaphore.wait(timeout: .now()) == .success {}

        // Phase 2: tear down dual-publish state when a foreign owner
        // takes over. Cancel any in-flight FileTransferCoordinator
        // downloads (writePromiseTo: callbacks already in flight will
        // see their FileDownloadTask cancelled and surface
        // FileDownloadError.cancelled to AppKit, which closes Finder's
        // progress dialog without a half-written file). The providers
        // array is dropped so retain cycles don't keep dead promises
        // alive past their useful lifetime.
        if let tid = activeTransferID {
            fileTransferCoordinator?.cancelTransfer(tid)
        }
        activePromiseProviders.removeAll()
        activeTransferID = nil
    }

    // MARK: - Private

    private func checkForChanges() {
        let currentCount = pasteboard.changeCount
        guard currentCount != lastChangeCount else { return }
        let prev = lastChangeCount
        lastChangeCount = currentCount
        // Diagnostic: every time we observe a changeCount bump that
        // is NOT our own, we log it.  If the leviathan→shen→helper
        // path is somehow triggering a CLIPBOARD_CHANGED echo back to
        // shen (which would then re-announce to leviathan = the loop
        // the user suspects), it'd show up here.
        Log.info(
            "checkForChanges: changeCount \(prev) → \(currentCount), lastSetHash=\(lastSetHash ?? "<nil>")"
        )

        let data = readPasteboard()

        // Suppress echo: don't report changes we caused ourselves
        if let lastHash = lastSetHash, data.contentHash == lastHash {
            Log.info("checkForChanges: echo-suppressed (matches lastSetHash)")
            return
        }

        guard data.contentType != .unspecified else {
            Log.info("checkForChanges: unspecified contentType (likely our own files announce skipped by defensive guard) — not emitting CLIPBOARD_CHANGED")
            return
        }

        Log.info("Local clipboard changed: \(data.contentType) hash=\(data.contentHash)")
        onClipboardChanged?(data)
    }

    private func readPasteboard() -> Leviathan_ClipboardData {
        var data = Leviathan_ClipboardData()

        // Defensive: if the pasteboard carries our own delayed-render
        // file marker, this is announceDelayedFiles' publication — calling
        // readObjects(forClasses: [NSURL.self]) below would synchronously
        // fire our own .fileURL data provider, kicking off a remote
        // download at announce time.  Skip the URL read entirely; the
        // changeCount echo suppression in checkForChanges already returns
        // before reaching here in the normal case, but covering this with
        // a content-type check defends against any future code path that
        // calls readPasteboard without first checking changeCount.
        let fileIndexType = NSPasteboard.PasteboardType("com.leviathan.clipboard.file-index")
        if pasteboard.types?.contains(fileIndexType) == true {
            return data
        }

        // Type-priority probe order matters: many clipboard sources
        // (browsers, chat apps, email clients) ALSO write a text /
        // HTML representation alongside a real file or image — Safari's
        // "Copy Image" puts both `.png` and `.string` (the image URL or
        // alt text) on the pasteboard.  If we check text before file
        // URLs / image bytes, we misclassify these as text and ship
        // 400-byte HTML snippets to leviathan instead of the real
        // image.  Order: files → image (PNG / TIFF) → text → fallback.
        if let urls = pasteboard.readObjects(forClasses: [NSURL.self], options: [
            .urlReadingFileURLsOnly: true
        ]) as? [URL], !urls.isEmpty {
            let result = clipboardDataFromFileURLs(urls)
            localFileURLs = result.fileIDtoURL  // store for FILE_CHUNK_REQUEST serving
            return result.data
        }

        // Try image (PNG) BEFORE text — image-copy sources commonly
        // include a text representation that would otherwise win.
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

        // Try text (fallback after images — see header comment).
        if let text = pasteboard.string(forType: .string), !text.isEmpty {
            data.contentType = .text
            data.payload = Data(text.utf8)
            data.contentHash = sha256Hex(data.payload)
            return data
        }

        return data
    }

    /// Write the rendered payload to the pasteboard for `type`. Callers
    /// pass `cachedTiff` so we can avoid the expensive NSImage decode +
    /// tiffRepresentation when the cache has already been pre-rendered on
    /// the socket queue inside provideData().
    private func writeDataToPasteboard(_ data: Data,
                                       cachedTiff: Data?,
                                       type: NSPasteboard.PasteboardType,
                                       announcement: Leviathan_ClipboardAnnouncement) {
        switch announcement.contentType {
        case .text:
            if let text = String(data: data, encoding: .utf8) {
                pasteboard.setString(text, forType: type)
            }
        case .image:
            if type == .tiff {
                // Prefer the pre-rendered cache. Falling back to an
                // on-demand decode only happens if the background
                // conversion failed (rare; logged in provideData).
                if let cached = cachedTiff {
                    pasteboard.setData(cached, forType: type)
                } else if let image = NSImage(data: data),
                          let tiffData = image.tiffRepresentation {
                    pasteboard.setData(tiffData, forType: type)
                } else {
                    pasteboard.setData(data, forType: type)
                }
            } else {
                pasteboard.setData(data, forType: type)
            }
        default:
            pasteboard.setData(data, forType: type)
        }
    }

}

// MARK: - Pure helpers (file scope for unit-testability)

/// Hex-encoded SHA-256 of `data`. Used as the content_hash field for
/// every ClipboardData and ClipboardAnnouncement we emit, so the parent
/// can dedupe and route DATA_REQUEST round-trips against a stable id.
///
/// File-scope so unit tests can exercise it without instantiating a
/// PasteboardManager (which would touch NSPasteboard on init).
func sha256Hex(_ data: Data) -> String {
    var hash = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
    data.withUnsafeBytes { ptr in
        _ = CC_SHA256(ptr.baseAddress, CC_LONG(data.count), &hash)
    }
    return hash.map { String(format: "%02x", $0) }.joined()
}

/// Build a Files-flavoured `Leviathan_ClipboardData` from a sequence of
/// local file URLs. Side effect: also returns the `file_id → URL` map
/// the caller stores on PasteboardManager.localFileURLs so the
/// FILE_CHUNK_REQUEST path can resolve the upstream-announced id back
/// to an on-disk path. Pure aside from the FileManager.attributesOfItem
/// stat call (testable against real temp files).
func clipboardDataFromFileURLs(
    _ urls: [URL]
) -> (data: Leviathan_ClipboardData, fileIDtoURL: [String: URL]) {
    var data = Leviathan_ClipboardData()
    data.contentType = .files
    var fileIDtoURL: [String: URL] = [:]
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
        fileIDtoURL[meta.fileID] = url
    }
    // The path-join hash is stable across runs as long as the URL set
    // and order are identical, so the parent can dedupe echo-back
    // CLIPBOARD_CHANGED frames against our own SET_CLIPBOARD writes.
    let pathString = urls.map { $0.path }.joined(separator: "\n")
    data.contentHash = sha256Hex(Data(pathString.utf8))
    return (data, fileIDtoURL)
}

/// Decide whether an incoming HelperProvideData belongs to the current
/// pending announcement. The "no pending announcement at all" case is
/// the caller's responsibility (guard let pending = pendingAnnouncement
/// before calling); this rule only encapsulates the hash-match check
/// so it can evolve (empty-hash rejection, expiry, etc.) in one place.
/// Pure: no I/O, no state.
func shouldAcceptProvideData(
    pending: Leviathan_ClipboardAnnouncement,
    incomingHash: String
) -> Bool {
    return pending.contentHash == incomingHash
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

        // Diagnostic: identify what's actually reading our `.fileURL`.
        //
        // macOS deliberately hides the caller from data-provider
        // callbacks — there's no `auditToken` or `pid_t` exposed, so we
        // can't directly name the reader.  But we CAN gather indirect
        // evidence:
        //
        //   * frontmostApp — user-focused window (NOT the reader; the
        //     reader can be a background daemon while shen is focused)
        //   * tSinceAnnounce — < 50 ms strongly suggests a synchronous
        //     system service (Continuity, indexers) that hooks into
        //     the pasteboard server's change-count bump.  > 1 s is
        //     more consistent with a polling clipboard manager OR a
        //     real user Cmd+V
        //   * changeCount delta — if pasteboard.changeCount > our
        //     lastChangeCount, somebody ELSE wrote to the pasteboard
        //     between our announce and now (third-party clipboard
        //     manager echoing a transient copy)
        //   * thread name / call stack symbols — sometimes the AppKit
        //     dispatch path leaves a fingerprint (e.g. "PBSync" /
        //     "ContinuityClipboard" in the stack indicates Continuity)
        //   * known-running clipboard processes — `pboard`, `pbs`,
        //     known managers — filter NSWorkspace.runningApplications
        let frontmost = NSWorkspace.shared.frontmostApplication
        let frontmostName = frontmost?.localizedName ?? "<nil>"
        let frontmostBundle = frontmost?.bundleIdentifier ?? "<nil>"
        let frontmostPid = frontmost?.processIdentifier ?? -1
        let timeSinceAnnounce: TimeInterval
        if let t = lastAnnounceTime {
            timeSinceAnnounce = -t.timeIntervalSinceNow
        } else {
            timeSinceAnnounce = -1
        }
        // `self.pasteboard` (the instance property — non-optional NSPasteboard)
        // — the `pasteboard` parameter to this NSPasteboardItemDataProvider
        // callback is optional (`NSPasteboard?`) and shadows the property
        // name inside this function.
        let liveChangeCount = self.pasteboard.changeCount
        let changeCountDelta = liveChangeCount - lastChangeCount
        let thread = Thread.current
        let threadName = thread.name ?? ""
        let threadDescription = thread.description
        // Walk the first ~20 stack frames — AppKit / pasteboard server
        // dispatch paths often carry a tell-tale symbol like
        // `_pasteboard_pbpaste`, `__NSCallback`, or
        // `_CFPasteboardSyncWithRemote`.  Truncate to keep the log line
        // sane; the symbols are typically the most informative ones.
        let stackSymbols = Thread.callStackSymbols.prefix(20).joined(separator: " | ")
        // List clipboard-suspect processes that are running right now
        // (so we can correlate which one most plausibly fired the read).
        let suspects = NSWorkspace.shared.runningApplications.compactMap { app -> String? in
            guard let bundle = app.bundleIdentifier?.lowercased() else { return nil }
            let suspectKeywords = [
                "pboard", "pbs", "continuity",
                "maccy", "raycast", "alfred", "paste",
                "copyclip", "pastebot", "flycut",
            ]
            for keyword in suspectKeywords where bundle.contains(keyword) {
                return "\(app.bundleIdentifier ?? "?")(pid=\(app.processIdentifier))"
            }
            return nil
        }.joined(separator: ", ")

        Log.info("""
            File data provider: requested file at index \(index)
              frontmost='\(frontmostName)' bundle=\(frontmostBundle) pid=\(frontmostPid)
              tSinceAnnounce=\(String(format: "%.3f", timeSinceAnnounce))s
              changeCount=\(liveChangeCount) (delta from last write: \(changeCountDelta))
              thread='\(threadName)' \(threadDescription)
              clipboardSuspects=[\(suspects)]
              stack=\(stackSymbols)
            """)

        // Probe suppression via early-return was tried (commits c13afc1
        // / e90c8b1) and reverted — `NSPasteboardItem.provideDataForType:`
        // is contractually called AT MOST ONCE per (item, type), and the
        // pasteboard caches the result (including an empty one when the
        // provider returns without calling `setString`).  A probe at
        // T+4 ms therefore consumed the single allowed call and cached
        // nil, so the user's real Cmd+V minutes later read nil from the
        // cache and Finder's Paste lit up "empty" / failed silently.
        //
        // Probe defence now relies on two non-cache-poisoning paths:
        //   1. `org.nspasteboard.source = com.aethersea.clipboard-helper`
        //      pasteboard type — well-behaved clipboard managers (Maccy,
        //      Paste, Raycast) check this and skip when the user has
        //      added our identifier to their ignore list.
        //   2. The `org.nspasteboard.{Transient,Concealed,AutoGenerated}Type`
        //      markers on every `.fileURL` item — handles managers that
        //      filter on those community-convention markers without
        //      requiring a user-side ignore-list entry.
        //
        // Continuity / Universal Clipboard does NOT honour either set of
        // markers (Apple system service); accept the bandwidth cost of
        // its probe-time download in exchange for the user's real Cmd+V
        // working from the resulting cache hit.  See plan-mac-virtual-
        // file-clipboard.md for the long-form trade-off discussion.

        // ACTUAL user-paste moment for the legacy `.fileURL` path — pop
        // the panel now (the eager pre-download trigger this method
        // also feeds into doesn't go anywhere near onTransferStart).
        // For Finder consumers that use the NSFilePromiseProvider path
        // instead, FilePromiseHandler.writePromiseTo fires its own
        // onTransferStart.
        let transferID = announcement.transferID
        DispatchQueue.main.async { [weak self] in
            self?.onTransferStart?(transferID)
        }

        guard let paths = ensureFilesDownloaded(announcement: announcement) else {
            Log.error("File data provider: ensureFilesDownloaded returned nil (download failed or timed out)")
            return
        }

        guard index < paths.count else {
            Log.error("File data provider: index \(index) out of range (have \(paths.count) path(s))")
            return
        }

        let filePath = paths[index]
        let url = URL(fileURLWithPath: filePath)

        // Verify the file actually exists before handing the URL to the OS
        if !FileManager.default.fileExists(atPath: filePath) {
            Log.error("File data provider: file does not exist at path: \(filePath)")
        }

        item.setString(url.absoluteString, forType: .fileURL)
        Log.info("Provided file URL: \(filePath)")
    }

    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {
        Log.info("Pasteboard finished with file data provider")
    }

    /// Mark the in-flight file download as cancelled.  Wired by
    /// ClipboardHelperApp to the TransferProgressPanel's onCancel
    /// callback.  Both ensureFilesDownloaded spin loops poll the
    /// `fileDownloadCancelled` flag and exit promptly when it flips.
    ///
    /// `transferID` is the ID to use for the upstream FILE_TRANSFER_CANCEL
    /// IPC message.  The caller in ClipboardHelperApp prefers the
    /// dcTransfer session UUID (captured from FILE_TRANSFER_PROGRESS
    /// frames) over the announcement's content_hash, because leviathan's
    /// dcTransfer session map is keyed by the UUID.  If empty, falls
    /// back to the announcement's transferID so something at least
    /// propagates upstream.
    ///
    /// Previously this routine carried a "stale transferID" check that
    /// rejected any ID not matching `pendingAnnouncement.transferID`.
    /// That broke once `transferID` started arriving as the dcTransfer
    /// UUID (which intentionally differs from the announcement's content
    /// hash), turning the cancel into a silent no-op.  Stale-cancel
    /// protection now lives in panel-lifecycle management (the panel is
    /// closed before a new announcement opens its replacement), so
    /// dropping the check here is safe.
    func cancelFileDownload(transferID: String) {
        let currentTransferID: String = {
            stateLock.lock()
            defer { stateLock.unlock() }
            return pendingAnnouncement?.transferID ?? ""
        }()

        let cancelTransferID = transferID.isEmpty ? currentTransferID : transferID

        Log.info("cancelFileDownload: marking download cancelled (transfer=\(cancelTransferID))")
        fileDownloadCondition.lock()
        fileDownloadCancelled = true
        fileDownloadCondition.broadcast()
        fileDownloadCondition.unlock()
        // Also poke the render semaphore so the primary waiter's
        // wait(timeout:) returns promptly instead of having to wait
        // for the next 50 ms tick.
        renderSemaphore.signal()

        // Notify upstream so shen-mac (and leviathan) can stop the
        // in-flight dcTransfer and reclaim bandwidth.  Without this
        // the helper-local cancel still leaves the WebRTC peer
        // streaming the rest of the file to disk that no one will
        // read.  Uses the existing HELPER_MESSAGE_TYPE_ERROR channel
        // with a structured prefix so we don't need a proto change;
        // shen-mac's helper-message handler watches for the prefix.
        if !cancelTransferID.isEmpty {
            onCancelTransfer?(cancelTransferID)
        }
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

        let isStillCurrent: () -> Bool = { [weak self] in
            guard let self = self else { return false }
            self.stateLock.lock()
            defer { self.stateLock.unlock() }
            return self.pendingAnnouncement?.contentHash == announcement.contentHash
        }

        // First caller triggers the download
        if !fileDownloadTriggered {
            fileDownloadTriggered = true
            fileDownloadCondition.unlock()

            // Send DATA_REQUEST to parent process.  NOTE: do NOT fire
            // onTransferStart here.  ensureFilesDownloaded is called
            // BOTH from the user-paste path (.fileURL data provider)
            // AND from the announcement-time eager pre-download below
            // (announceDelayedFiles dispatches it on a background queue
            // so the legacy `.fileURL` consumers have their data ready
            // by the time they actually paste).  Firing the panel here
            // would pop the panel at announcement time — way before
            // the user actually pasted — which is the wrong UX.
            // Panel show is now triggered by the actual user-paste
            // sites (FilePromiseHandler.writePromiseTo for the modern
            // path, the `.fileURL` data provider callback for the
            // legacy path).
            var request = Leviathan_ClipboardDataRequest()
            request.contentHash = announcement.contentHash
            request.contentType = .files
            onDataRequest?(request)

            // Wait for PROVIDE_DATA while keeping the main run loop alive.
            // 50 ms spin so FileTransferProgress updates (driving the
            // TransferProgressPanel) keep flowing on DispatchQueue.main.
            // Also polls fileDownloadCancelled so the user's Cancel
            // button click exits this loop promptly.
            let deadline = Date(timeIntervalSinceNow: 300)
            var completed = false
            var superseded = false
            var cancelled = false
            while !completed && !superseded && !cancelled && Date() < deadline {
                if isFileDownloadCancelled() {
                    cancelled = true
                } else if renderSemaphore.wait(timeout: .now()) == .success {
                    // Re-check cancellation: the signal might have been
                    // posted by cancelFileDownload (which also signals)
                    // rather than by genuine PROVIDE_DATA completion.
                    cancelled = isFileDownloadCancelled()
                    completed = !cancelled
                } else if !isStillCurrent() {
                    superseded = true
                } else if Thread.isMainThread {
                    // `.common` (not `.default`) — macOS dispatches mouse /
                    // button-click events in `.eventTracking` mode, which
                    // `.default` does NOT include.  Spinning in `.default`
                    // left the user's Cancel click queued forever (beach-
                    // ball) until the spin loop exited on its own timeout.
                    // `.common` is the meta-mode that automatically covers
                    // any mode registered via NSRunLoop's commonModes —
                    // includes `.default` AND `.eventTracking` AND modal
                    // panel modes — so the Cancel button reaches its
                    // target/action during this 50 ms slice.
                    RunLoop.current.run(mode: .common, before: Date(timeIntervalSinceNow: 0.05))
                } else {
                    Thread.sleep(forTimeInterval: 0.05)
                }
            }

            fileDownloadCondition.lock()
            if completed, isStillCurrent(),
               let data = renderedData,
               let str = String(data: data, encoding: .utf8) {
                fileDownloadPaths = str.components(separatedBy: "\n").filter { !$0.isEmpty }
                Log.info("File download complete: \(fileDownloadPaths?.count ?? 0) file(s)")
            } else if cancelled {
                Log.info("File download cancelled by user")
            } else if superseded {
                Log.warning("File download abandoned: announcement superseded mid-flight")
            } else {
                Log.error("File download failed or timed out")
            }
            fileDownloadCondition.broadcast()
            let paths = fileDownloadPaths
            fileDownloadCondition.unlock()
            return paths
        }

        // Another caller while download is in-flight — spin until done.
        fileDownloadCondition.unlock()
        let deadline = Date(timeIntervalSinceNow: 300)
        while Date() < deadline {
            fileDownloadCondition.lock()
            let paths = fileDownloadPaths
            let wasCancelled = fileDownloadCancelled
            fileDownloadCondition.unlock()
            if let paths = paths {
                return paths
            }
            if wasCancelled {
                Log.info("ensureFilesDownloaded (secondary): cancelled by user")
                return nil
            }
            if !isStillCurrent() {
                Log.warning("ensureFilesDownloaded (secondary): announcement superseded, exiting")
                return nil
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

    /// Lock-guarded read of the cancellation flag.  Pulled out so the
    /// spin loops above don't have to inline the lock/unlock pair on
    /// every iteration.
    private func isFileDownloadCancelled() -> Bool {
        fileDownloadCondition.lock()
        defer { fileDownloadCondition.unlock() }
        return fileDownloadCancelled
    }
}

