import AppKit
import Foundation

/// A floating HUD-style panel that shows file transfer progress.
/// Displayed by the clipboard-helper when the Go server reports download progress.
final class TransferProgressPanel {

    private var panel: NSPanel?
    private var progressIndicator: NSProgressIndicator?
    private var statusLabel: NSTextField?
    private var bytesLabel: NSTextField?

    // MARK: - Show / Close

    func showPanel() {
        guard panel == nil else { return }

        let panelWidth: CGFloat = 320
        let panelHeight: CGFloat = 90

        // Position: bottom-right of the main screen, above the Dock
        let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1920, height: 1080)
        let panelX = screenFrame.maxX - panelWidth - 16
        let panelY = screenFrame.minY + 16

        let p = NSPanel(
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

        let contentView = NSView(frame: NSRect(x: 0, y: 0, width: panelWidth, height: panelHeight))

        // Status label: "Transferring files…"
        let status = NSTextField(labelWithString: "Transferring files…")
        status.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        status.frame = NSRect(x: 16, y: 52, width: panelWidth - 32, height: 20)
        contentView.addSubview(status)
        statusLabel = status

        // Progress bar
        let progress = NSProgressIndicator(frame: NSRect(x: 16, y: 32, width: panelWidth - 32, height: 14))
        progress.style = .bar
        progress.isIndeterminate = false
        progress.minValue = 0
        progress.maxValue = 1.0
        progress.doubleValue = 0
        contentView.addSubview(progress)
        progressIndicator = progress

        // Bytes label: "0 KB / 0 KB"
        let bytes = NSTextField(labelWithString: "")
        bytes.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        bytes.textColor = .secondaryLabelColor
        bytes.frame = NSRect(x: 16, y: 8, width: panelWidth - 32, height: 16)
        contentView.addSubview(bytes)
        bytesLabel = bytes

        p.contentView = contentView
        p.orderFront(nil)
        panel = p
    }

    func close() {
        panel?.orderOut(nil)
        panel = nil
        progressIndicator = nil
        statusLabel = nil
        bytesLabel = nil
    }

    // MARK: - Update

    func updateProgress(bytesTransferred: UInt64, totalBytes: UInt64) {
        guard let progressIndicator else { return }

        let fraction = progressFraction(transferred: bytesTransferred, total: totalBytes)
        progressIndicator.doubleValue = fraction

        let transferred = formatBytes(bytesTransferred)
        let total = formatBytes(totalBytes)
        let pct = Int(fraction * 100)
        bytesLabel?.stringValue = "\(transferred) / \(total)  (\(pct)%)"
    }

    func completeTransfer(success: Bool, errorMessage: String) {
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
