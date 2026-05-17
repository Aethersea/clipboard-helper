import XCTest
import UniformTypeIdentifiers
@testable import ClipboardHelper

final class FilePromiseHandlerTests: XCTestCase {

    // MARK: - filePromiseUTI

    func testUTIFromKnownExtensionUsesExtensionMatch() {
        // A filename with a recognized extension takes the extension
        // path — UTType maps "pdf" to com.adobe.pdf.
        let uti = filePromiseUTI(forFilename: "report.pdf", mimeType: "")
        XCTAssertEqual(uti, UTType.pdf.identifier)
    }

    func testUTIExtensionTakesPrecedenceOverMime() {
        // Conflicting hints: extension says PDF, MIME says JSON. The
        // extension wins because it's the more specific signal and
        // matches what Finder shows in the icon.
        let uti = filePromiseUTI(forFilename: "doc.pdf",
                                 mimeType: "application/json")
        XCTAssertEqual(uti, UTType.pdf.identifier)
    }

    func testUTIFallsBackToMimeWhenNoExtension() {
        let uti = filePromiseUTI(forFilename: "blob",
                                 mimeType: "application/json")
        XCTAssertEqual(uti, UTType.json.identifier)
    }

    func testUTIDynamicallySynthesizedFromUnknownExtensionWinsOverMime() {
        // UTType doesn't return nil for an unknown extension — it
        // synthesises a `dyn.*` identifier. The production code
        // therefore takes the extension path and returns the dynamic
        // UTI, NOT the MIME-derived UTType.json. Lock in that
        // behaviour so a future caller that expected MIME-fallback
        // for novel extensions surfaces the surprise here.
        let uti = filePromiseUTI(forFilename: "x.zzznotreal",
                                 mimeType: "application/json")
        XCTAssertTrue(uti.hasPrefix("dyn."), "expected dyn.* UTI, got \(uti)")
        XCTAssertNotEqual(uti, UTType.json.identifier)
    }

    func testUTIBothEmptyReturnsPublicData() {
        let uti = filePromiseUTI(forFilename: "", mimeType: "")
        XCTAssertEqual(uti, UTType.data.identifier)
    }

    func testUTIFilenameWithoutExtensionAndEmptyMimeReturnsPublicData() {
        // NSString.pathExtension of "Makefile" is empty.
        let uti = filePromiseUTI(forFilename: "Makefile", mimeType: "")
        XCTAssertEqual(uti, UTType.data.identifier)
    }

    func testUTILeadingDotFilenameHasNoExtension() {
        // NSString.pathExtension of ".pdf" is "" — the dotfile rule
        // treats the whole name as the basename. Falls through to MIME
        // if present, else public.data.
        let utiNoMime = filePromiseUTI(forFilename: ".pdf", mimeType: "")
        XCTAssertEqual(utiNoMime, UTType.data.identifier)
        let utiWithMime = filePromiseUTI(forFilename: ".pdf",
                                         mimeType: "application/json")
        XCTAssertEqual(utiWithMime, UTType.json.identifier)
    }

    // MARK: - sanitizeFilename

    func testSanitizePlainNameReturnsSameName() {
        XCTAssertEqual(sanitizeFilename("plain.txt"), "plain.txt")
    }

    func testSanitizeStripsLeadingPathTraversal() {
        // The defining defense-in-depth case: a remote peer that sets
        // relative_path = "../../etc/passwd" must NOT be able to
        // influence the OS write destination.
        XCTAssertEqual(sanitizeFilename("../../etc/passwd"), "passwd")
    }

    func testSanitizeStripsAbsolutePath() {
        XCTAssertEqual(sanitizeFilename("/abs/path/to/file.bin"), "file.bin")
    }

    func testSanitizeStripsRelativePath() {
        XCTAssertEqual(sanitizeFilename("subdir/leaf"), "leaf")
    }

    func testSanitizeEmptyInputReturnsUntitled() {
        XCTAssertEqual(sanitizeFilename(""), "untitled")
    }

    func testSanitizeTrailingSlashUsesPriorComponent() {
        // NSString.lastPathComponent of "a/b/" strips the trailing
        // slash and returns "b", not the empty string.
        XCTAssertEqual(sanitizeFilename("a/b/"), "b")
    }

    func testSanitizeRootPathReturnsRootVerbatim() {
        // lastPathComponent of "/" is "/", which is non-empty, so the
        // sanitizer returns "/" verbatim. Document the behaviour so a
        // future tightening that maps "/" → "untitled" doesn't break
        // silently.
        XCTAssertEqual(sanitizeFilename("/"), "/")
    }

    func testSanitizeDotComponentsPassThroughVerbatim() {
        // The sanitizer relies on AppKit's destination-URL builder to
        // reject "." / ".." rather than mapping them here. Lock in the
        // behaviour so a regression that lets "." sneak into a write
        // path surfaces here, not in the field.
        XCTAssertEqual(sanitizeFilename("."), ".")
        XCTAssertEqual(sanitizeFilename(".."), "..")
        XCTAssertEqual(sanitizeFilename("subdir/.."), "..")
    }

    func testSanitizePreservesUnicodeBasename() {
        // CJK and emoji basenames must round-trip; AppKit handles UTF-16
        // filenames natively, so we don't transliterate.
        XCTAssertEqual(sanitizeFilename("dir/檔案.txt"), "檔案.txt")
        XCTAssertEqual(sanitizeFilename("dir/🍕.png"), "🍕.png")
    }
}
