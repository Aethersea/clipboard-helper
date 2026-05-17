import XCTest
@testable import ClipboardHelper

final class LogTests: XCTestCase {

    // Restore the production stderr sink + default verbose setting after
    // each test so a captured-sink leak can't bleed across cases.
    override func tearDown() {
        Log.writeSink = { FileHandle.standardError.write($0) }
        Log.verbose = false
        super.tearDown()
    }

    // MARK: - formatLine

    func testFormatLineHasIsoTimestampLevelTagAndMessage() {
        // 2026-01-01T00:00:00Z. ISO8601DateFormatter's default options
        // emit `[date]T[time]Z` so the timestamp prefix is deterministic.
        let date = Date(timeIntervalSince1970: 1_767_225_600)
        let line = Log.formatLine(level: "INFO", message: "hello", date: date)
        XCTAssertEqual(line, "[2026-01-01T00:00:00Z] [INFO] hello\n")
    }

    func testFormatLineLevelTagIsLiteral() {
        // The function does no level validation — whatever the caller
        // passes lands verbatim inside the brackets.
        let date = Date(timeIntervalSince1970: 0)
        let line = Log.formatLine(level: "SOMETHING", message: "x", date: date)
        XCTAssertTrue(line.contains("[SOMETHING]"))
    }

    func testFormatLineEmptyMessageStillTerminatesWithLineFeed() {
        let date = Date(timeIntervalSince1970: 0)
        let line = Log.formatLine(level: "INFO", message: "", date: date)
        XCTAssertTrue(line.hasSuffix(" \n"))
    }

    func testFormatLineMultilineMessagePreservedVerbatim() {
        // The formatter intentionally does not split or reformat embedded
        // newlines — multi-line stack traces or pretty-printed payloads
        // come through as-is.
        let date = Date(timeIntervalSince1970: 0)
        let line = Log.formatLine(level: "WARN", message: "first\nsecond", date: date)
        XCTAssertTrue(line.contains("first\nsecond"))
        XCTAssertTrue(line.hasSuffix("\n"))
    }

    // MARK: - I/O sink dispatch

    func testInfoEmitsLineThroughSink() {
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.info("hi")

        XCTAssertEqual(captured.count, 1)
        let line = String(data: captured[0], encoding: .utf8) ?? ""
        XCTAssertTrue(line.contains("[INFO] hi"))
    }

    func testWarningUsesWarnTag() {
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.warning("careful")

        XCTAssertEqual(captured.count, 1)
        let line = String(data: captured[0], encoding: .utf8) ?? ""
        XCTAssertTrue(line.contains("[WARN] careful"))
    }

    func testErrorUsesErrorTag() {
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.error("boom")

        XCTAssertEqual(captured.count, 1)
        let line = String(data: captured[0], encoding: .utf8) ?? ""
        XCTAssertTrue(line.contains("[ERROR] boom"))
    }

    // MARK: - Verbose gate on debug

    func testDebugSuppressedWhenVerboseFalse() {
        // verbose=false is the production default; debug must NOT touch
        // the sink so non-verbose runs keep stderr quiet.
        Log.verbose = false
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.debug("trace")

        XCTAssertTrue(captured.isEmpty)
    }

    func testDebugEmittedWhenVerboseTrue() {
        Log.verbose = true
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.debug("trace")

        XCTAssertEqual(captured.count, 1)
        let line = String(data: captured[0], encoding: .utf8) ?? ""
        XCTAssertTrue(line.contains("[DEBUG] trace"))
    }

    func testInfoAndWarnAndErrorAreNotGatedByVerbose() {
        // Only debug is gated. The other three must emit whether verbose
        // is on or off.
        Log.verbose = false
        var captured: [Data] = []
        Log.writeSink = { captured.append($0) }

        Log.info("a")
        Log.warning("b")
        Log.error("c")

        XCTAssertEqual(captured.count, 3)
    }
}
