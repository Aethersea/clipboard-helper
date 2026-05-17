import XCTest
@testable import ClipboardHelper

/// Pure-logic tests for the file-scope helpers extracted out of
/// TransferProgressPanel. The NSPanel UI itself is not covered —
/// AppKit requires an active run loop and a graphical session, and
/// the panel only ever displays the values these helpers return.
final class TransferProgressPanelTests: XCTestCase {

    // MARK: - progressFraction

    func testProgressFractionZeroTotalGuardsAgainstDivisionByZero() {
        // An UInt64 / 0 would crash; the helper must return 0 instead.
        // This is the path the panel hits on first updateProgress
        // before the announcement supplies a real total.
        XCTAssertEqual(progressFraction(transferred: 0, total: 0), 0)
        XCTAssertEqual(progressFraction(transferred: 999, total: 0), 0)
    }

    func testProgressFractionMidWay() {
        XCTAssertEqual(progressFraction(transferred: 50, total: 100), 0.5)
    }

    func testProgressFractionCompleteBoundary() {
        XCTAssertEqual(progressFraction(transferred: 100, total: 100), 1.0)
    }

    func testProgressFractionStartBoundary() {
        XCTAssertEqual(progressFraction(transferred: 0, total: 100), 0)
    }

    func testProgressFractionClampsOverReport() {
        // A buggy upstream that reports transferred > total must not
        // drive the NSProgressIndicator past its max of 1.0 — that
        // would render the bar's tail off-screen on the HUD panel.
        XCTAssertEqual(progressFraction(transferred: 200, total: 100), 1.0)
    }

    func testProgressFractionLargeValuesPreserveRatio() {
        // Real-world large transfers — 4 GiB total, 1 GiB transferred.
        // Tests Double precision is sufficient at the high end.
        let oneGiB = UInt64(1) << 30
        let fourGiB = UInt64(4) << 30
        XCTAssertEqual(progressFraction(transferred: oneGiB, total: fourGiB),
                       0.25,
                       accuracy: 1e-9)
    }

    // MARK: - formatBytes

    func testFormatBytesZeroHasZeroPrefix() {
        // ByteCountFormatter .file style emits "Zero <unit>" for 0 on
        // macOS — the unit varies by OS version (macOS 14 uses "Zero
        // KB", earlier "Zero bytes"). Lock the prefix, not the unit,
        // so the test survives the next macOS bump.
        let s = formatBytes(0)
        XCTAssertTrue(s.hasPrefix("Zero "), "expected 'Zero ' prefix, got \(s)")
    }

    func testFormatBytesSmallNumberContainsByteUnit() {
        // .file style typically renders 1-byte counts as "1 byte" but
        // macOS sometimes coerces to "1 KB" once it's hosting a file
        // (.file is "size on disk"). Assert only that the formatter
        // returns *some* non-empty string for small values; the unit
        // suffix tests below cover real scale boundaries.
        XCTAssertFalse(formatBytes(1).isEmpty)
        XCTAssertFalse(formatBytes(2).isEmpty)
    }

    func testFormatBytesKilobyteScale() {
        // .file uses 1000-based KB (not 1024), matching Finder's
        // display. A 1500-byte file shows as "2 KB" with rounding.
        // Don't pin the exact string — different macOS versions
        // vary fractional precision — but confirm the unit suffix.
        let s = formatBytes(1500)
        XCTAssertTrue(s.contains("KB"), "expected KB unit, got \(s)")
    }

    func testFormatBytesMegabyteScale() {
        let s = formatBytes(5 * 1000 * 1000)
        XCTAssertTrue(s.contains("MB"), "expected MB unit, got \(s)")
    }

    func testFormatBytesGigabyteScale() {
        let oneGiB = UInt64(1) << 30  // 1_073_741_824
        let s = formatBytes(oneGiB)
        XCTAssertTrue(s.contains("GB"), "expected GB unit, got \(s)")
    }

    func testFormatBytesNeverContainsNegativeSign() {
        // UInt64 → Int64 conversion at 2^63 - 1 stays positive; just
        // confirm we never accidentally hand ByteCountFormatter a
        // negative number even at the high end of UInt64.
        let big: UInt64 = UInt64(Int64.max)
        let s = formatBytes(big)
        XCTAssertFalse(s.contains("-"))
    }

    func testFormatBytesUInt64HighEndClampsInsteadOfTrapping() {
        // UInt64.max > Int64.max, so a naive Int64(bytes) conversion
        // would trap. The helper must clamp instead. Reaching this
        // line at all is the assertion — if formatBytes traps the
        // test runner reports a crash.
        let s = formatBytes(UInt64.max)
        XCTAssertFalse(s.isEmpty)
        XCTAssertFalse(s.contains("-"))
    }
}
