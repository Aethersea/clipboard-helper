import XCTest
import ArgumentParser
@testable import ClipboardHelper

final class HelperModeTests: XCTestCase {

    func testServerArgumentParsesToServerCase() {
        XCTAssertEqual(HelperMode(argument: "server"), .server)
    }

    func testClientArgumentParsesToClientCase() {
        XCTAssertEqual(HelperMode(argument: "client"), .client)
    }

    func testInvalidArgumentReturnsNil() {
        XCTAssertNil(HelperMode(argument: ""))
        XCTAssertNil(HelperMode(argument: "Server"))   // case-sensitive
        XCTAssertNil(HelperMode(argument: "daemon"))
    }

    func testDescriptionMatchesRawValue() {
        XCTAssertEqual(HelperMode.server.description, "server")
        XCTAssertEqual(HelperMode.client.description, "client")
    }

    func testAllCasesParseRoundTrip() {
        for mode in [HelperMode.server, .client] {
            XCTAssertEqual(HelperMode(argument: mode.rawValue), mode)
            XCTAssertEqual(HelperMode(argument: mode.description), mode)
        }
    }
}
