import XCTest
@testable import ClipboardHelper

final class FileTransferCoordinatorTests: XCTestCase {

    private var coordinator: FileTransferCoordinator!
    private var tempDir: URL!
    private let lock = NSLock()
    private var sentMessages: [Leviathan_HelperMessage] = []

    override func setUp() {
        super.setUp()
        coordinator = FileTransferCoordinator()
        tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("clipboard-helper-test-\(UUID().uuidString)")
        try! FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        sentMessages = []
        coordinator.sendMessage = { [weak self] msg in
            guard let self else { return }
            self.lock.lock()
            self.sentMessages.append(msg)
            self.lock.unlock()
        }
    }

    override func tearDown() {
        coordinator = nil
        if let tempDir {
            try? FileManager.default.removeItem(at: tempDir)
        }
        tempDir = nil
        super.tearDown()
    }

    // MARK: - Helpers

    private func chunkRequests() -> [Leviathan_HelperFileChunkRequest] {
        lock.lock()
        defer { lock.unlock() }
        return sentMessages.compactMap { msg in
            if case .fileChunkRequest(let r) = msg.payload { return r }
            return nil
        }
    }

    @discardableResult
    private func waitForChunkRequests(atLeast count: Int, timeout: TimeInterval) -> [Leviathan_HelperFileChunkRequest] {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let reqs = chunkRequests()
            if reqs.count >= count { return reqs }
            Thread.sleep(forTimeInterval: 0.02)
        }
        return chunkRequests()
    }

    // MARK: - Announcement registration

    func testRegisterAnnouncementStoresFileMetadata() {
        let ann = TestHelpers.makeAnnouncement(
            transferID: "tx1",
            contentHash: "hash1",
            files: [
                TestHelpers.makeFileMetadata(id: "fA", name: "a.txt", size: 11),
                TestHelpers.makeFileMetadata(id: "fB", name: "b.bin", size: 2222),
            ]
        )
        coordinator.registerAnnouncement(ann)

        let a = coordinator.fileMetadata(transferID: "tx1", fileID: "fA")
        XCTAssertEqual(a?.filename, "a.txt")
        XCTAssertEqual(a?.fileSize, 11)

        let b = coordinator.fileMetadata(transferID: "tx1", fileID: "fB")
        XCTAssertEqual(b?.filename, "b.bin")
        XCTAssertEqual(b?.fileSize, 2222)

        XCTAssertNil(coordinator.fileMetadata(transferID: "tx1", fileID: "missing"))
        XCTAssertNil(coordinator.fileMetadata(transferID: "unknown", fileID: "fA"))
    }

    // MARK: - Empty file (size = 0)

    func testEmptyFileCreatesZeroByteFileAndSendsNoChunkRequests() {
        let destURL = tempDir.appendingPathComponent("empty.bin")
        let exp = expectation(description: "completion")

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f0",
            fileSize: 0,
            to: destURL,
            parentProgress: Progress(),
            completion: { err in
                XCTAssertNil(err)
                exp.fulfill()
            }
        )

        wait(for: [exp], timeout: 2.0)

        XCTAssertTrue(FileManager.default.fileExists(atPath: destURL.path))
        let data = try? Data(contentsOf: destURL)
        XCTAssertEqual(data?.count, 0)
        XCTAssertTrue(chunkRequests().isEmpty, "Empty file must not trigger any chunk requests")
    }

    // MARK: - Pipeline depth

    func testInitialPipelineWindowMatchesDepth() {
        let destURL = tempDir.appendingPathComponent("pipe.bin")
        // Big enough that the pipeline depth is the binding constraint, not the file size.
        let fileSize = UInt64(TestHelpers.chunkSize * TestHelpers.pipelineDepth * 3)

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: fileSize,
            to: destURL,
            parentProgress: Progress(),
            completion: { _ in }
        )

        let initial = waitForChunkRequests(atLeast: TestHelpers.pipelineDepth, timeout: 2.0)
        XCTAssertEqual(initial.count, TestHelpers.pipelineDepth)

        // No more requests should arrive without feeding chunks back.
        Thread.sleep(forTimeInterval: 0.2)
        XCTAssertEqual(chunkRequests().count, TestHelpers.pipelineDepth)

        let expectedOffsets = (0..<TestHelpers.pipelineDepth).map { UInt64($0 * TestHelpers.chunkSize) }
        XCTAssertEqual(initial.map { $0.offset }, expectedOffsets)
        XCTAssertTrue(initial.allSatisfy { $0.size == UInt32(TestHelpers.chunkSize) })
        XCTAssertTrue(initial.allSatisfy { $0.transferID == "tx" && $0.fileID == "f1" })
    }

    func testPipelineRefillsAsChunksArrive() {
        let destURL = tempDir.appendingPathComponent("refill.bin")
        let totalChunks = TestHelpers.pipelineDepth + 2
        let payload = TestHelpers.payload(byteCount: totalChunks * TestHelpers.chunkSize)

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: UInt64(payload.count),
            to: destURL,
            parentProgress: Progress(),
            completion: { _ in }
        )

        _ = waitForChunkRequests(atLeast: TestHelpers.pipelineDepth, timeout: 2.0)
        XCTAssertEqual(chunkRequests().count, TestHelpers.pipelineDepth)

        // Feed one chunk → pipeline should top up by one request.
        let firstSlice = TestHelpers.slice(of: payload, offset: 0, maxLength: TestHelpers.chunkSize)
        coordinator.handleChunkData(TestHelpers.makeChunk(
            transferID: "tx", fileID: "f1", offset: 0, data: firstSlice))

        let after = waitForChunkRequests(atLeast: TestHelpers.pipelineDepth + 1, timeout: 2.0)
        XCTAssertEqual(after.count, TestHelpers.pipelineDepth + 1)
        XCTAssertEqual(after.last?.offset, UInt64(TestHelpers.pipelineDepth * TestHelpers.chunkSize))
    }

    // MARK: - Full happy path

    func testFullDownloadWritesPayloadAndCompletes() throws {
        let destURL = tempDir.appendingPathComponent("full.bin")
        // Pick a size that crosses the pipeline window and ends on a partial trailing chunk.
        let fileSize = TestHelpers.chunkSize * (TestHelpers.pipelineDepth + 2) - 7
        let payload = TestHelpers.payload(byteCount: fileSize)
        let exp = expectation(description: "completion")
        var completionError: Error?

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: UInt64(payload.count),
            to: destURL,
            parentProgress: Progress(),
            completion: { err in
                completionError = err
                exp.fulfill()
            }
        )

        // Feed chunks at the standard chunk grid. Coordinator's serial queue
        // processes them in order after start() opens the file.
        var offset: UInt64 = 0
        while offset < UInt64(payload.count) {
            let data = TestHelpers.slice(of: payload, offset: offset, maxLength: TestHelpers.chunkSize)
            let next = offset + UInt64(data.count)
            coordinator.handleChunkData(TestHelpers.makeChunk(
                transferID: "tx", fileID: "f1",
                offset: offset, data: data,
                isLast: next >= UInt64(payload.count)))
            offset = next
        }

        wait(for: [exp], timeout: 5.0)
        XCTAssertNil(completionError)

        let onDisk = try Data(contentsOf: destURL)
        XCTAssertEqual(onDisk, payload)
    }

    // MARK: - Out-of-order chunk delivery

    func testOutOfOrderChunkDeliveryStillProducesCorrectFile() throws {
        let destURL = tempDir.appendingPathComponent("ooo.bin")
        let totalChunks = TestHelpers.pipelineDepth
        let payload = TestHelpers.payload(byteCount: totalChunks * TestHelpers.chunkSize)
        let exp = expectation(description: "completion")

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: UInt64(payload.count),
            to: destURL,
            parentProgress: Progress(),
            completion: { err in
                XCTAssertNil(err)
                exp.fulfill()
            }
        )

        // Build chunks then deliver them in reverse order so the terminal
        // chunk (isLast=true) arrives before earlier ones — verifies that
        // completion does not fire on isLast alone.
        let offsets = (0..<totalChunks).map { UInt64($0 * TestHelpers.chunkSize) }
        for offset in offsets.reversed() {
            let data = TestHelpers.slice(of: payload, offset: offset, maxLength: TestHelpers.chunkSize)
            let isTerminal = offset + UInt64(data.count) >= UInt64(payload.count)
            coordinator.handleChunkData(TestHelpers.makeChunk(
                transferID: "tx", fileID: "f1",
                offset: offset, data: data, isLast: isTerminal))
        }

        wait(for: [exp], timeout: 5.0)

        let onDisk = try Data(contentsOf: destURL)
        XCTAssertEqual(onDisk, payload)
    }

    // MARK: - Server-side error chunk

    func testServerErrorChunkPropagatesErrorToCompletion() {
        let destURL = tempDir.appendingPathComponent("err.bin")
        let exp = expectation(description: "completion")

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: UInt64(TestHelpers.chunkSize),
            to: destURL,
            parentProgress: Progress(),
            completion: { err in
                XCTAssertNotNil(err)
                XCTAssertTrue(err!.localizedDescription.contains("disk read failed"),
                              "Error should surface the server-reported reason; got: \(err!.localizedDescription)")
                exp.fulfill()
            }
        )

        coordinator.handleChunkData(TestHelpers.makeChunk(
            transferID: "tx", fileID: "f1",
            offset: 0, data: Data(),
            isLast: true,
            error: "disk read failed"))

        wait(for: [exp], timeout: 2.0)
    }

    // MARK: - Cancel

    func testCancelTransferFiresCompletionWithCancellationError() {
        let destURL = tempDir.appendingPathComponent("cancel.bin")
        let exp = expectation(description: "completion")

        coordinator.startFileDownload(
            transferID: "tx",
            fileID: "f1",
            fileSize: UInt64(TestHelpers.chunkSize * 4),
            to: destURL,
            parentProgress: Progress(),
            completion: { err in
                XCTAssertNotNil(err)
                XCTAssertTrue(err!.localizedDescription.lowercased().contains("cancel"))
                exp.fulfill()
            }
        )

        _ = waitForChunkRequests(atLeast: 1, timeout: 1.0)
        coordinator.cancelTransfer("tx")

        wait(for: [exp], timeout: 2.0)
    }

    func testCancelTransferIsScopedToTransferID() {
        let destA = tempDir.appendingPathComponent("a.bin")
        let destB = tempDir.appendingPathComponent("b.bin")
        let expA = expectation(description: "A cancelled")
        let expB = expectation(description: "B completed")

        coordinator.startFileDownload(
            transferID: "txA", fileID: "f",
            fileSize: UInt64(TestHelpers.chunkSize * 2),
            to: destA, parentProgress: Progress(),
            completion: { err in
                XCTAssertNotNil(err)
                expA.fulfill()
            }
        )
        coordinator.startFileDownload(
            transferID: "txB", fileID: "f",
            fileSize: 0,
            to: destB, parentProgress: Progress(),
            completion: { err in
                XCTAssertNil(err)
                expB.fulfill()
            }
        )

        _ = waitForChunkRequests(atLeast: 1, timeout: 1.0)
        coordinator.cancelTransfer("txA")

        wait(for: [expA, expB], timeout: 2.0)
    }

    // MARK: - Completion idempotency

    func testCompletionFiresExactlyOnceEvenIfCancelArrivesAfterSuccess() {
        let destURL = tempDir.appendingPathComponent("idem.bin")
        let exp = expectation(description: "completion")
        exp.assertForOverFulfill = true
        var callCount = 0
        let countLock = NSLock()

        coordinator.startFileDownload(
            transferID: "tx", fileID: "f",
            fileSize: 0,
            to: destURL, parentProgress: Progress(),
            completion: { _ in
                countLock.lock()
                callCount += 1
                countLock.unlock()
                exp.fulfill()
            }
        )

        wait(for: [exp], timeout: 1.0)

        // Cancel after the task already completed; must not fire completion again.
        coordinator.cancelTransfer("tx")
        Thread.sleep(forTimeInterval: 0.2)

        countLock.lock()
        XCTAssertEqual(callCount, 1)
        countLock.unlock()
    }

    // MARK: - Failure to create destination

    func testStartFailsWhenDestinationParentMissing() {
        // tempDir/missing-parent never gets created — createFile and the
        // subsequent FileHandle(forWritingTo:) both fail.
        let bogusDest = tempDir.appendingPathComponent("missing-parent/file.bin")
        let exp = expectation(description: "completion")

        coordinator.startFileDownload(
            transferID: "tx", fileID: "f",
            fileSize: UInt64(TestHelpers.chunkSize),
            to: bogusDest,
            parentProgress: Progress(),
            completion: { err in
                XCTAssertNotNil(err, "Opening a non-creatable destination must report an error")
                exp.fulfill()
            }
        )

        wait(for: [exp], timeout: 2.0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: bogusDest.path))
    }

    // MARK: - Chunk request format

    func testEmittedChunkRequestsUseTheChosenChunkSize() {
        coordinator.startFileDownload(
            transferID: "tx", fileID: "f",
            fileSize: UInt64(TestHelpers.chunkSize) * 4,
            to: tempDir.appendingPathComponent("fmt.bin"),
            parentProgress: Progress(),
            completion: { _ in }
        )

        let reqs = waitForChunkRequests(atLeast: 1, timeout: 1.0)
        XCTAssertFalse(reqs.isEmpty)
        for req in reqs {
            XCTAssertEqual(req.size, UInt32(TestHelpers.chunkSize))
            XCTAssertEqual(req.transferID, "tx")
            XCTAssertEqual(req.fileID, "f")
        }

        // The first emitted IPC message must be a FILE_CHUNK_REQUEST envelope.
        lock.lock()
        let first = sentMessages.first
        lock.unlock()
        XCTAssertEqual(first?.type, .fileChunkRequest)
    }
}
