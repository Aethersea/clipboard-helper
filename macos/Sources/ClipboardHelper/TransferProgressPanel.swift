import AppKit
import Foundation

/// NSPanel subclass that overrides `canBecomeKey` to return true.
///
/// Required because TransferProgressPanel uses `.nonactivatingPanel` in
/// the styleMask so popping the panel doesn't yank focus away from
/// whichever app the user is in.  The default NSPanel behaviour for
/// `.nonactivatingPanel` is `canBecomeKey == false`, which is fine for
/// purely-display HUDs — but our HUD has a Cancel `NSButton`, and
/// `NSButton`'s click → `target/action` dispatch needs the panel to be
/// a key window for the click to flow through the responder chain.
/// Without this override the Cancel button visually depresses (or not,
/// depending on appearance) but the `@objc cancelButtonClicked` selector
/// never fires.
///
/// Combined with `.nonactivatingPanel`: app itself does NOT activate
/// (helper stays a background daemon, user's foreground app keeps
/// focus), but the panel CAN become key when the user clicks inside it
/// — which is exactly what's needed to route Cancel.
private final class ClickableHUDPanel: NSPanel {
    override var canBecomeKey: Bool { true }
    // Stay out of the main-window pool — only key, never main.  Main
    // status is for document-style windows, not floating HUDs.
    override var canBecomeMain: Bool { false }
}

/// A floating HUD-style panel that shows file transfer progress.
/// Displayed by the clipboard-helper when the Go server reports download progress.
final class TransferProgressPanel: NSObject {

    /// User clicked the Cancel button in the HUD.  Wired by
    /// ClipboardHelperApp at panel-creation time so the cancellation
    /// flows back to PasteboardManager (which sets a flag the
    /// ensureFilesDownloaded spin loop checks).
    var onCancel: (() -> Void)?

    private var panel: NSPanel?
    private var progressIndicator: NSProgressIndicator?
    private var statusLabel: NSTextField?
    private var bytesLabel: NSTextField?
    private var cancelButton: NSButton?

    // MARK: - Show / Close

    func showPanel() {
        guard panel == nil else { return }

        let panelWidth: CGFloat = 360
        let panelHeight: CGFloat = 100

        // Position: bottom-right of the main screen, above the Dock
        let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1920, height: 1080)
        let panelX = screenFrame.maxX - panelWidth - 16
        let panelY = screenFrame.minY + 16

        let p = ClickableHUDPanel(
            contentRect: NSRect(x: panelX, y: panelY, width: panelWidth, height: panelHeight),
            styleMask: [.titled, .hudWindow, .utilityWindow, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        p.title = "File Transfer"
        p.isFloatingPanel = true
        p.hidesOnDeactivate = false
        p.level = .floating
        p.collectionBehavior = [.canJoinAllSpaces, .transient]
        p.isMovableByWindowBackground = true
        // Leave `becomesKeyOnlyIfNeeded` at its default (false) and
        // explicitly makeKey below.  An earlier version set this to
        // true thinking it'd avoid stealing focus, but combined with
        // `.nonactivatingPanel` + `.hudWindow` the side effect was that
        // the panel never became key on display, which made AppKit
        // draw the Cancel `NSButton` in its non-key/inactive style —
        // the button looked permanently greyed-out / disabled even
        // though `isEnabled == true`.
        //
        // `.nonactivatingPanel` still applies: the helper APP does NOT
        // activate (system frontmost stays on the user's paste-target
        // app); only the panel's intra-app key state flips, which is
        // what NSButton needs to render in its active style.

        let contentView = NSView(frame: NSRect(x: 0, y: 0, width: panelWidth, height: panelHeight))

        // Cancel button (top-right).  Sized for HUD aesthetic;
        // `bezelStyle = .inline` matches macOS native cancel-during-
        // copy chrome.  Width fits "Cancel" in en_US — localise the
        // title separately if a future locale needs more room.
        let cancelButtonWidth: CGFloat = 64
        let cancelButtonHeight: CGFloat = 22
        let cancel = NSButton(frame: NSRect(
            x: panelWidth - 16 - cancelButtonWidth,
            y: panelHeight - 16 - cancelButtonHeight,
            width: cancelButtonWidth,
            height: cancelButtonHeight))
        cancel.title = "Cancel"
        cancel.bezelStyle = .inline
        cancel.font = NSFont.systemFont(ofSize: 11)
        cancel.target = self
        cancel.action = #selector(cancelButtonClicked)
        contentView.addSubview(cancel)
        cancelButton = cancel

        // Status label: "Transferring files…" (left of cancel button)
        let status = NSTextField(labelWithString: "Transferring files…")
        status.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        status.frame = NSRect(x: 16, y: 60, width: panelWidth - 32 - cancelButtonWidth - 8, height: 20)
        contentView.addSubview(status)
        statusLabel = status

        // Progress bar — starts indeterminate so the user sees motion
        // immediately when the panel pops up, before the first real
        // FILE_TRANSFER_PROGRESS frame arrives.  Switches to determinate
        // mode on the first updateProgress(bytesTransferred:totalBytes:)
        // call.
        let progress = NSProgressIndicator(frame: NSRect(x: 16, y: 36, width: panelWidth - 32, height: 14))
        progress.style = .bar
        progress.isIndeterminate = true
        progress.minValue = 0
        progress.maxValue = 1.0
        progress.doubleValue = 0
        progress.startAnimation(nil)
        contentView.addSubview(progress)
        progressIndicator = progress

        // Bytes label: "0 KB / 0 KB"
        let bytes = NSTextField(labelWithString: "")
        bytes.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        bytes.textColor = .secondaryLabelColor
        bytes.frame = NSRect(x: 16, y: 10, width: panelWidth - 32, height: 16)
        contentView.addSubview(bytes)
        bytesLabel = bytes

        p.contentView = contentView
        // `orderFront` alone doesn't make the panel key (it just brings
        // it visually forward).  Without key status, NSButton inside a
        // `.hudWindow` panel renders in its non-key style which looks
        // disabled.  `makeKey()` flips the panel to key WITHIN our
        // process — the `.nonactivatingPanel` styleMask ensures the
        // helper app itself does NOT activate / steal system frontmost
        // from the user's paste-target app.
        p.orderFront(nil)
        p.makeKey()
        panel = p
    }

    @objc private func cancelButtonClicked() {
        // Reflect the cancel intent immediately in the UI before the
        // upstream pipeline observes the flag and tears down — gives
        // the user instant feedback that the click registered.
        statusLabel?.stringValue = "Cancelling…"
        cancelButton?.isEnabled = false
        onCancel?()
    }

    func close() {
        panel?.orderOut(nil)
        panel = nil
        progressIndicator = nil
        statusLabel = nil
        bytesLabel = nil
        cancelButton = nil
        onCancel = nil
    }

    // MARK: - Update

    func updateProgress(bytesTransferred: UInt64, totalBytes: UInt64) {
        guard let progressIndicator else { return }

        // First real progress frame — flip from indeterminate (which
        // showed motion while the transfer was bootstrapping) to a
        // determinate bar driven by actual byte counts.
        if progressIndicator.isIndeterminate {
            progressIndicator.stopAnimation(nil)
            progressIndicator.isIndeterminate = false
            statusLabel?.stringValue = "Transferring files…"
        }

        let fraction = progressFraction(transferred: bytesTransferred, total: totalBytes)
        progressIndicator.doubleValue = fraction

        let transferred = formatBytes(bytesTransferred)
        let total = formatBytes(totalBytes)
        let pct = Int(fraction * 100)
        bytesLabel?.stringValue = "\(transferred) / \(total)  (\(pct)%)"
    }

    /// Reset the panel back to "in-flight, indeterminate" state.  Used
    /// when a multi-file paste rolls from one file's completion straight
    /// into the next file's start within the auto-dismiss window — we
    /// want to keep the same window on screen (no flicker) but undo the
    /// "Transfer complete"/disabled-Cancel terminal state from the
    /// previous file, otherwise the user sees a stale terminal panel
    /// while the next file is silently downloading.
    func resetForNextTransfer() {
        guard let progressIndicator else { return }
        statusLabel?.stringValue = "Transferring files…"
        cancelButton?.isEnabled = true
        bytesLabel?.stringValue = ""
        progressIndicator.doubleValue = 0
        if !progressIndicator.isIndeterminate {
            progressIndicator.isIndeterminate = true
            progressIndicator.startAnimation(nil)
        }
    }

    func completeTransfer(success: Bool, errorMessage: String) {
        cancelButton?.isEnabled = false
        // Tidy up any leftover indeterminate animation — the panel may
        // have gone straight from showPanel() to completeTransfer()
        // without a single intermediate updateProgress (small / fast
        // transfers where the only progress frame is the terminal
        // is_complete=true one).
        if progressIndicator?.isIndeterminate == true {
            progressIndicator?.stopAnimation(nil)
            progressIndicator?.isIndeterminate = false
        }
        if success {
            statusLabel?.stringValue = "Transfer complete"
            progressIndicator?.doubleValue = 1.0
            bytesLabel?.stringValue = ""
        } else {
            statusLabel?.stringValue = "Transfer failed"
            bytesLabel?.stringValue = errorMessage
        }
    }
}

// MARK: - Pure helpers (file scope for unit-testability)

/// Format a byte count for display via ByteCountFormatter's `.file`
/// style. Matches what Finder shows in the inspector ("1.5 MB", "2 KB",
/// etc.). File-scope so unit tests don't have to construct an NSPanel.
///
/// Note: ByteCountFormatter localises unit names against the current
/// process locale ("octet" in fr_FR, etc.) and does NOT expose a
/// `locale` property to pin it. Unit tests that assert on exact
/// strings ("Zero bytes", "1 byte") therefore rely on the CI runner
/// (GitHub Actions macos-14) defaulting to en_US. If a future change
/// switches the runner locale, the assertions need loosening to unit
/// suffix checks.
func formatBytes(_ bytes: UInt64) -> String {
    let formatter = ByteCountFormatter()
    formatter.countStyle = .file
    // Int64 conversion would trap for bytes >= 2^63. APFS can't host
    // a single file that large today, but defending the conversion
    // here keeps the helper safe against any future protocol that
    // sends UInt64-wide values from a non-filesystem source.
    let clamped = Int64(min(bytes, UInt64(Int64.max)))
    return formatter.string(fromByteCount: clamped)
}

/// Map (transferred, total) to a progress fraction in [0.0, 1.0].
/// Guards against division-by-zero by returning 0 when total is zero,
/// and clamps to 1.0 if a buggy upstream over-reports transferred.
func progressFraction(transferred: UInt64, total: UInt64) -> Double {
    guard total > 0 else { return 0 }
    let raw = Double(transferred) / Double(total)
    // UInt64 / UInt64 is non-negative so a lower bound of 0 is
    // already implicit; only the upper bound needs enforcement.
    return min(raw, 1.0)
}
