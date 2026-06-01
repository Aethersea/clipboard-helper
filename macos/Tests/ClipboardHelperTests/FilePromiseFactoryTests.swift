import AppKit
import XCTest
@testable import ClipboardHelper

/// Tests for the `NSFilePromiseProvider` factory + the delegate's
/// error-path completion contract in FilePromiseHandler.swift.
///
/// These exercise the instance-method surface (unlike the pure free
/// functions `sanitizeFilename` / `filePromiseUTI`, which have their own
/// tests). They stay headless-safe: `PasteboardManager.init` only reads
/// `NSPasteboard.general.changeCount` (no polling timer until
/// `startPolling()`), and constructing an `NSFilePromiseProvider` needs no
/// window server. We never call `startPolling()`.
final class FilePromiseFactoryTests: XCTestCase {

    private func makeMetadata(
        fileID: String = "7",
        filename: String = "report.pdf",
        fileSize: UInt64 = 4096,
        mimeType: String = "application/pdf"
    ) -> Leviathan_FileMetadata {
        var meta = Leviathan_FileMetadata()
        meta.fileID = fileID
        meta.filename = filename
        meta.fileSize = fileSize
        meta.mimeType = mimeType
        return meta
    }

    // MARK: - makeFilePromiseProvider

    func testMakeFilePromiseProviderPopulatesUserInfo() {
        let pm = PasteboardManager()
        let meta = makeMetadata(fileID: "42", filename: "notes.txt",
                                fileSize: 1234, mimeType: "text/plain")

        let provider = pm.makeFilePromiseProvider(for: meta, transferID: "xfer-abc")

        let info = provider.userInfo as? [String: Any]
        XCTAssertNotNil(info)
        XCTAssertEqual(info?[kPromiseUserInfoFileID] as? String, "42")
        XCTAssertEqual(info?[kPromiseUserInfoTransferID] as? String, "xfer-abc")
        XCTAssertEqual(info?[kPromiseUserInfoFileSize] as? UInt64, 1234)
        XCTAssertEqual(info?[kPromiseUserInfoFilename] as? String, "notes.txt")
    }

    func testMakeFilePromiseProviderDerivesFileTypeFromFilenameExtension() {
        let pm = PasteboardManager()
        let meta = makeMetadata(filename: "archive.zip", mimeType: "")

        let provider = pm.makeFilePromiseProvider(for: meta, transferID: "t")

        // The provider's fileType must match the same UTI resolution the
        // pure helper produces for this filename/MIME pair.
        XCTAssertEqual(provider.fileType,
                       filePromiseUTI(forFilename: "archive.zip", mimeType: ""))
    }

    // MARK: - writePromiseTo error-path completion contract
    //
    // Both error paths must fire completionHandler exactly once and
    // synchronously, otherwise Finder's copy panel spins forever.

    func testWritePromiseToMalformedUserInfoCompletesWithMalformedError() {
        let pm = PasteboardManager()
        // A promise whose userInfo lacks the required keys — the delegate
        // must reject it rather than start a download.
        let provider = NSFilePromiseProvider(fileType: UTType.data.identifier,
                                             delegate: pm)
        provider.userInfo = [String: Any]()  // empty → missing fileID/transferID/size

        let dest = FileManager.default.temporaryDirectory
            .appendingPathComponent("clipboard-helper-promise-\(UUID().uuidString)")

        var called = false
        var captured: Error?
        pm.filePromiseProvider(provider, writePromiseTo: dest) { err in
            called = true
            captured = err
        }

        XCTAssertTrue(called, "completionHandler must fire synchronously on malformed userInfo")
        guard let fpe = captured as? FilePromiseError else {
            return XCTFail("expected FilePromiseError, got \(String(describing: captured))")
        }
        guard case .malformedUserInfo = fpe else {
            return XCTFail("expected .malformedUserInfo, got \(fpe)")
        }
    }

    func testWritePromiseToWithoutCoordinatorCompletesWithCoordinatorUnavailable() {
        let pm = PasteboardManager()
        // fileTransferCoordinator is nil by default (AppDelegate normally
        // sets it during launch); a valid-userInfo promise must therefore
        // fail with .coordinatorUnavailable, not hang.
        XCTAssertNil(pm.fileTransferCoordinator)

        let provider = pm.makeFilePromiseProvider(
            for: makeMetadata(), transferID: "xfer-1")

        let dest = FileManager.default.temporaryDirectory
            .appendingPathComponent("clipboard-helper-promise-\(UUID().uuidString)")

        var called = false
        var captured: Error?
        pm.filePromiseProvider(provider, writePromiseTo: dest) { err in
            called = true
            captured = err
        }

        XCTAssertTrue(called, "completionHandler must fire synchronously when coordinator is missing")
        guard let fpe = captured as? FilePromiseError else {
            return XCTFail("expected FilePromiseError, got \(String(describing: captured))")
        }
        guard case .coordinatorUnavailable = fpe else {
            return XCTFail("expected .coordinatorUnavailable, got \(fpe)")
        }
    }
}
