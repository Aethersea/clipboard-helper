import XCTest
@testable import ClipboardHelper

/// Pure-logic tests for the file-scope helpers extracted out of
/// PasteboardManager. The NSPasteboard read/write itself is not
/// covered — those paths need a real graphical session and stay
/// manual-smoke.
final class PasteboardManagerUnitTests: XCTestCase {

    // MARK: - sha256Hex

    func testSha256HexEmptyInputMatchesKnownDigest() {
        // Known SHA-256 of "" is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        let hex = sha256Hex(Data())
        XCTAssertEqual(hex,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    }

    func testSha256HexAbcMatchesKnownDigest() {
        // Known SHA-256 of "abc" is ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
        let hex = sha256Hex(Data("abc".utf8))
        XCTAssertEqual(hex,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }

    func testSha256HexIsDeterministicForSameInput() {
        let payload = Data("repeat me".utf8)
        XCTAssertEqual(sha256Hex(payload), sha256Hex(payload))
    }

    func testSha256HexProducesLowercaseHexDigest() {
        // 64 hex chars, all 0-9a-f.
        let hex = sha256Hex(Data("anything".utf8))
        XCTAssertEqual(hex.count, 64)
        let allowed = CharacterSet(charactersIn: "0123456789abcdef")
        XCTAssertTrue(hex.unicodeScalars.allSatisfy { allowed.contains($0) })
    }

    // MARK: - clipboardDataFromFileURLs

    func testClipboardDataFromFileURLsSingleFileMetadata() throws {
        let tmp = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: tmp) }
        let payload = Data("hello world".utf8)
        let file = tmp.appendingPathComponent("one.txt")
        try payload.write(to: file)

        let result = clipboardDataFromFileURLs([file])
        XCTAssertEqual(result.data.contentType, .files)
        XCTAssertEqual(result.data.files.count, 1)
        let meta = result.data.files[0]
        XCTAssertEqual(meta.fileID, "0")
        XCTAssertEqual(meta.filename, "one.txt")
        XCTAssertEqual(meta.relativePath, file.path)
        XCTAssertEqual(meta.fileSize, UInt64(payload.count))
        XCTAssertFalse(meta.isDirectory)
        XCTAssertEqual(result.fileIDtoURL["0"], file)
    }

    func testClipboardDataFromFileURLsMissingFileReportsZeroSize() {
        // FileManager.attributesOfItem throws for a missing path. The
        // helper must not propagate the error — fall back to size=0 and
        // keep going so a degenerate URL doesn't poison the whole
        // CLIPBOARD_CHANGED snapshot.
        let bogus = URL(fileURLWithPath: "/tmp/this-path-should-never-exist-clipboard-helper-test")
        let result = clipboardDataFromFileURLs([bogus])
        XCTAssertEqual(result.data.files.count, 1)
        XCTAssertEqual(result.data.files[0].fileSize, 0)
    }

    func testClipboardDataFromFileURLsMultipleFilesPreserveOrderAndIds() throws {
        let tmp = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: tmp) }
        let a = tmp.appendingPathComponent("a.bin")
        let b = tmp.appendingPathComponent("b.bin")
        let c = tmp.appendingPathComponent("c.bin")
        for (i, url) in [a, b, c].enumerated() {
            try Data([UInt8(i)]).write(to: url)
        }

        let result = clipboardDataFromFileURLs([a, b, c])
        XCTAssertEqual(result.data.files.map(\.fileID), ["0", "1", "2"])
        XCTAssertEqual(result.data.files.map(\.filename), ["a.bin", "b.bin", "c.bin"])
        XCTAssertEqual(result.fileIDtoURL["0"], a)
        XCTAssertEqual(result.fileIDtoURL["1"], b)
        XCTAssertEqual(result.fileIDtoURL["2"], c)
    }

    func testClipboardDataFromFileURLsDirectoryFlag() throws {
        let tmp = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: tmp) }
        let dir = tmp.appendingPathComponent("subdir", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false)

        let result = clipboardDataFromFileURLs([dir])
        XCTAssertTrue(result.data.files[0].isDirectory)
    }

    func testClipboardDataFromFileURLsHashIsDeterministicForSamePaths() throws {
        let tmp = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: tmp) }
        let a = tmp.appendingPathComponent("dupe-a.txt")
        let b = tmp.appendingPathComponent("dupe-b.txt")
        for url in [a, b] { try Data().write(to: url) }

        let first  = clipboardDataFromFileURLs([a, b])
        let second = clipboardDataFromFileURLs([a, b])
        XCTAssertEqual(first.data.contentHash, second.data.contentHash)
        XCTAssertFalse(first.data.contentHash.isEmpty)
    }

    func testClipboardDataFromFileURLsHashDiffersWhenOrderDiffers() throws {
        // Order matters — the path-join hash is sensitive to ordering
        // so the parent can echo-back-suppress only when the exact
        // sequence matches the write.
        let tmp = try makeTempDir()
        defer { try? FileManager.default.removeItem(at: tmp) }
        let a = tmp.appendingPathComponent("ordered-a.txt")
        let b = tmp.appendingPathComponent("ordered-b.txt")
        for url in [a, b] { try Data().write(to: url) }

        let forward = clipboardDataFromFileURLs([a, b])
        let reverse = clipboardDataFromFileURLs([b, a])
        XCTAssertNotEqual(forward.data.contentHash, reverse.data.contentHash)
    }

    // MARK: - shouldAcceptProvideData

    func testShouldAcceptProvideDataAcceptsMatchingHash() {
        var pending = Leviathan_ClipboardAnnouncement()
        pending.contentHash = "feedbeef"
        XCTAssertTrue(shouldAcceptProvideData(pending: pending,
                                              incomingHash: "feedbeef"))
    }

    func testShouldAcceptProvideDataRejectsMismatchedHash() {
        var pending = Leviathan_ClipboardAnnouncement()
        pending.contentHash = "feedbeef"
        XCTAssertFalse(shouldAcceptProvideData(pending: pending,
                                               incomingHash: "deadbeef"))
    }

    func testShouldAcceptProvideDataRejectsCaseMismatch() {
        // Hashes are emitted lowercase hex; a case-mismatched incoming
        // hash signals a re-encoding bug somewhere upstream and must
        // be rejected rather than silently treated as equal.
        var pending = Leviathan_ClipboardAnnouncement()
        pending.contentHash = "abcd"
        XCTAssertFalse(shouldAcceptProvideData(pending: pending,
                                               incomingHash: "ABCD"))
    }

    // MARK: - Helpers

    private func makeTempDir() throws -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("clipboard-helper-pbm-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
}
