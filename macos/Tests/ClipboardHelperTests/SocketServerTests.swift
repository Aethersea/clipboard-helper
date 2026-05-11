import XCTest
import Darwin
@testable import ClipboardHelper

final class SocketServerTests: XCTestCase {

    private var server: SocketServer!
    private var socketPath: String!
    private let lock = NSLock()
    private var receivedMessages: [Leviathan_HelperMessage] = []

    override func setUp() {
        super.setUp()
        // Keep the path short — sockaddr_un.sun_path is 104 chars on macOS.
        socketPath = "/tmp/ch-test-\(UUID().uuidString.prefix(8)).sock"
        receivedMessages = []
    }

    override func tearDown() {
        server?.stop()
        server = nil
        if let socketPath {
            unlink(socketPath)
        }
        socketPath = nil
        super.tearDown()
    }

    // MARK: - Helpers

    private func startServer(file: StaticString = #file, line: UInt = #line) throws {
        server = SocketServer(socketPath: socketPath)
        server.onMessage = { [weak self] msg in
            guard let self else { return }
            self.lock.lock()
            self.receivedMessages.append(msg)
            self.lock.unlock()
        }
        try server.start()
    }

    private func connectClient(file: StaticString = #file, line: UInt = #line) throws -> Int32 {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        XCTAssertGreaterThanOrEqual(fd, 0, "socket() failed errno=\(errno)", file: file, line: line)

        // Cap blocking reads so a stuck server can't hang the test.
        var tv = timeval(tv_sec: 3, tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = socketPath.utf8CString
        XCTAssertLessThanOrEqual(pathBytes.count, MemoryLayout.size(ofValue: addr.sun_path),
                                 "socket path too long", file: file, line: line)
        withUnsafeMutablePointer(to: &addr.sun_path) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: pathBytes.count) { dest in
                for (i, b) in pathBytes.enumerated() { dest[i] = b }
            }
        }

        // Wait briefly for the listening socket to exist before connecting.
        let deadline = Date().addingTimeInterval(1.0)
        while !FileManager.default.fileExists(atPath: socketPath) && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.01)
        }

        let result = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                Darwin.connect(fd, sockPtr, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        XCTAssertEqual(result, 0, "connect() failed errno=\(errno)", file: file, line: line)
        return fd
    }

    private func framedBytes(for message: Leviathan_HelperMessage) throws -> Data {
        let body = try message.serializedData()
        var length = UInt32(body.count).littleEndian
        var out = Data(bytes: &length, count: 4)
        out.append(body)
        return out
    }

    @discardableResult
    private func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        return data.withUnsafeBytes { raw -> Bool in
            guard let base = raw.baseAddress else { return false }
            var written = 0
            while written < data.count {
                let n = Darwin.write(fd, base + written, data.count - written)
                if n <= 0 { return false }
                written += n
            }
            return true
        }
    }

    private func waitForMessages(count: Int, timeout: TimeInterval) -> [Leviathan_HelperMessage] {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            lock.lock()
            let snapshot = receivedMessages
            lock.unlock()
            if snapshot.count >= count { return snapshot }
            Thread.sleep(forTimeInterval: 0.02)
        }
        lock.lock()
        defer { lock.unlock() }
        return receivedMessages
    }

    private func makeReadyMessage(timestamp: UInt64 = 1) -> Leviathan_HelperMessage {
        var msg = Leviathan_HelperMessage()
        msg.type = .ready
        msg.timestamp = timestamp
        return msg
    }

    // MARK: - Lifecycle

    func testStartCreatesSocketFileAndStopRemovesIt() throws {
        try startServer()
        XCTAssertTrue(FileManager.default.fileExists(atPath: socketPath))

        server.stop()
        // unlink runs synchronously inside stop().
        XCTAssertFalse(FileManager.default.fileExists(atPath: socketPath))
    }

    func testStartFailsWhenPathTooLong() throws {
        let longName = String(repeating: "a", count: 200)
        let longPath = "/tmp/\(longName).sock"
        let s = SocketServer(socketPath: longPath)
        XCTAssertThrowsError(try s.start()) { err in
            guard case SocketError.pathTooLong = err else {
                XCTFail("Expected SocketError.pathTooLong, got \(err)")
                return
            }
        }
    }

    // MARK: - Frame parsing

    func testServerDecodesSingleFrame() throws {
        try startServer()
        let client = try connectClient()
        defer { close(client) }

        let frame = try framedBytes(for: makeReadyMessage(timestamp: 42))
        XCTAssertTrue(writeAll(client, frame))

        let received = waitForMessages(count: 1, timeout: 2.0)
        XCTAssertEqual(received.count, 1)
        XCTAssertEqual(received.first?.type, .ready)
        XCTAssertEqual(received.first?.timestamp, 42)
    }

    func testServerDecodesTwoFramesInOneWrite() throws {
        try startServer()
        let client = try connectClient()
        defer { close(client) }

        var combined = try framedBytes(for: makeReadyMessage(timestamp: 1))
        combined.append(try framedBytes(for: makeReadyMessage(timestamp: 2)))
        XCTAssertTrue(writeAll(client, combined))

        let received = waitForMessages(count: 2, timeout: 2.0)
        XCTAssertEqual(received.count, 2)
        XCTAssertEqual(received.map { $0.timestamp }, [1, 2])
    }

    func testServerBuffersFrameAcrossWrites() throws {
        try startServer()
        let client = try connectClient()
        defer { close(client) }

        let frame = try framedBytes(for: makeReadyMessage(timestamp: 99))
        // Split halfway through the body so both halves are partial frames.
        let split = frame.count / 2
        XCTAssertGreaterThan(split, 4, "Frame must be larger than the length prefix")

        XCTAssertTrue(writeAll(client, frame.subdata(in: 0..<split)))
        // Confirm no message materialises from a partial frame.
        Thread.sleep(forTimeInterval: 0.15)
        lock.lock()
        XCTAssertTrue(receivedMessages.isEmpty)
        lock.unlock()

        XCTAssertTrue(writeAll(client, frame.subdata(in: split..<frame.count)))
        let received = waitForMessages(count: 1, timeout: 2.0)
        XCTAssertEqual(received.count, 1)
        XCTAssertEqual(received.first?.timestamp, 99)
    }

    func testOversizedFramePrefixCausesDisconnect() throws {
        try startServer()

        let connectedExp = expectation(description: "connected")
        let disconnectExp = expectation(description: "disconnected")
        server.onClientConnected = { connectedExp.fulfill() }
        server.onClientDisconnected = { disconnectExp.fulfill() }

        let client = try connectClient()
        defer { close(client) }
        wait(for: [connectedExp], timeout: 2.0)

        // 65 MB — the server's frame cap is 64 MB.
        var length = UInt32(65 * 1024 * 1024).littleEndian
        let prefix = Data(bytes: &length, count: 4)
        XCTAssertTrue(writeAll(client, prefix))

        wait(for: [disconnectExp], timeout: 2.0)
    }

    // MARK: - Send path

    func testSendEmitsLengthPrefixedFrameToClient() throws {
        server = SocketServer(socketPath: socketPath)
        server.onMessage = { [weak self] msg in
            guard let self else { return }
            self.lock.lock()
            self.receivedMessages.append(msg)
            self.lock.unlock()
        }
        // Block send() until the server has actually accepted the client.
        // Otherwise the send races accept and the message gets dropped.
        let connectedExp = expectation(description: "client accepted")
        server.onClientConnected = { connectedExp.fulfill() }
        try server.start()

        let client = try connectClient()
        defer { close(client) }
        wait(for: [connectedExp], timeout: 2.0)

        var msg = Leviathan_HelperMessage()
        msg.type = .clipboardContent
        msg.timestamp = 7
        var inner = Leviathan_ClipboardData()
        inner.contentType = .text
        inner.contentHash = "abc"
        inner.payload = Data("hi".utf8)
        msg.payload = .clipboardData(inner)
        server.send(msg)

        // Read length prefix.
        var lengthBytes = [UInt8](repeating: 0, count: 4)
        let n = readExact(fd: client, into: &lengthBytes, count: 4, timeout: 2.0)
        XCTAssertEqual(n, 4, "Expected to read 4-byte length prefix")
        let length = UInt32(lengthBytes[0])
            | (UInt32(lengthBytes[1]) << 8)
            | (UInt32(lengthBytes[2]) << 16)
            | (UInt32(lengthBytes[3]) << 24)
        XCTAssertGreaterThan(length, 0)
        XCTAssertLessThan(length, 1024)  // sanity

        var bodyBytes = [UInt8](repeating: 0, count: Int(length))
        let m = readExact(fd: client, into: &bodyBytes, count: Int(length), timeout: 2.0)
        XCTAssertEqual(m, Int(length))

        let decoded = try Leviathan_HelperMessage(serializedBytes: Data(bodyBytes))
        XCTAssertEqual(decoded.type, .clipboardContent)
        XCTAssertEqual(decoded.timestamp, 7)
        if case .clipboardData(let cd) = decoded.payload {
            XCTAssertEqual(cd.contentHash, "abc")
            XCTAssertEqual(cd.contentType, .text)
            XCTAssertEqual(cd.payload, Data("hi".utf8))
        } else {
            XCTFail("Expected clipboardData payload, got \(decoded.payload as Any)")
        }
    }

    // MARK: - Low-level read with timeout

    private func readExact(fd: Int32, into buffer: UnsafeMutablePointer<UInt8>, count: Int, timeout: TimeInterval) -> Int {
        // The client fd has SO_RCVTIMEO set in connectClient(), so each
        // read() returns at most after a few seconds. The wall-clock
        // deadline below is an upper bound across all reads.
        let deadline = Date().addingTimeInterval(timeout)
        var total = 0
        while total < count {
            let remaining = count - total
            let n = Darwin.read(fd, buffer + total, remaining)
            if n > 0 {
                total += n
                continue
            }
            if n == 0 { return total }   // EOF
            // n < 0: timeout (EAGAIN/EWOULDBLOCK) or other error.
            if Date() > deadline { return total }
        }
        return total
    }
}
