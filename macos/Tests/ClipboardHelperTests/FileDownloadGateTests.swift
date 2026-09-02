import XCTest
@testable import ClipboardHelper

// Rules 10–17 pin the field-bug fixes and attempt identity:
//
// 10. Empty PROVIDE_DATA (and newlines-only) is `.failed`, not a cached
//     `.completed([])`. `parsePathList` still returns `[]` for those
//     payloads; the gate, not the parser, rejects them.
// 11. `timeout` is a stall bound refreshed by `noteProgress()`. Progress
//     recorded before a wait starts does not extend it; `reset()` clears
//     the mark.
// 12. A primary that returns `.timedOut` / `.failed` re-arms the gate
//     (`hasTriggered` is false) and publishes that outcome to waiting
//     secondaries promptly. `.cancelled` / `.superseded` do not re-arm.
// 13. `currentGeneration` advances by exactly one when a primary starts
//     and by exactly one on `reset()`. A secondary joining, a cached
//     read, `cancel()` and `noteProgress()` do not change it.
// 14. `reset()` during an in-flight primary wait makes that primary
//     return `.superseded` (not `.timedOut`) without touching state:
//     `hasTriggered` is false, `cachedPaths` is nil, and a later
//     semaphore signal / payload from the old attempt is never published.
// 15. `reset()` during an in-flight secondary wait makes that secondary
//     return `.superseded` promptly (within a couple of poll intervals).
// 16. ABA: a secondary that joined attempt g must return `.superseded`
//     (not the old failure, not the new primary's result) if a newer
//     primary has started before it next polls. It must not send.
// 17. Unchanged from 12b: a secondary that does observe its own
//     primary's failure, with no newer primary yet, still inherits that
//     exact outcome.

final class FileDownloadGateTests: XCTestCase {

    override func setUp() {
        super.setUp()
        Log.writeSink = { _ in }
    }

    override func tearDown() {
        Log.writeSink = { FileHandle.standardError.write($0) }
        super.tearDown()
    }

    // MARK: - Helpers

    @discardableResult
    private func waitUntil(
        timeout: TimeInterval,
        poll: TimeInterval = 0.01,
        file: StaticString = #file,
        line: UInt = #line,
        predicate: () -> Bool
    ) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            Thread.sleep(forTimeInterval: poll)
        }
        if predicate() { return true }
        XCTFail("condition not met within \(timeout)s", file: file, line: line)
        return false
    }

    private func invokeEnsure(
        _ gate: FileDownloadGate,
        semaphore: DispatchSemaphore,
        probe: GateProbe
    ) -> FileDownloadWaitOutcome {
        gate.ensureDownloaded(
            semaphore: semaphore,
            sendRequest: { probe.sendRequest() },
            isStillCurrent: { probe.isStillCurrent() },
            renderedPayload: { probe.renderedPayload() },
            idle: { probe.idle() }
        )
    }

    private func startWaiter(
        gate: FileDownloadGate,
        semaphore: DispatchSemaphore,
        probe: GateProbe,
        expectation exp: XCTestExpectation,
        outcome: OutcomeBox
    ) {
        DispatchQueue.global(qos: .userInitiated).async {
            outcome.set(self.invokeEnsure(gate, semaphore: semaphore, probe: probe))
            exp.fulfill()
        }
    }

    private func requireOutcome(
        _ box: OutcomeBox,
        file: StaticString = #file,
        line: UInt = #line
    ) -> FileDownloadWaitOutcome? {
        guard let outcome = box.get() else {
            XCTFail("waiter did not record an outcome", file: file, line: line)
            return nil
        }
        return outcome
    }

    private func assertOutcome(
        _ box: OutcomeBox,
        _ expected: FileDownloadWaitOutcome,
        file: StaticString = #file,
        line: UInt = #line
    ) {
        guard let outcome = requireOutcome(box, file: file, line: line) else { return }
        XCTAssertEqual(outcome, expected, file: file, line: line)
    }

    private func cancelAndWait(_ gate: FileDownloadGate, _ expectations: XCTestExpectation...) {
        gate.cancel()
        wait(for: expectations, timeout: 2.0)
    }

    /// Drive `noteProgress()` from the test thread on a schedule while a
    /// waiter runs on a background queue. Fails if any watched waiter
    /// returns while progress is still being recorded.
    private func pulseProgress(
        _ gate: FileDownloadGate,
        every interval: TimeInterval,
        for duration: TimeInterval,
        watching boxes: [OutcomeBox],
        file: StaticString = #file,
        line: UInt = #line
    ) {
        let deadline = Date().addingTimeInterval(duration)
        while Date() < deadline {
            for box in boxes {
                if let got = box.get() {
                    XCTFail("waiter returned \(got) while progress was still being recorded",
                            file: file, line: line)
                    return
                }
            }
            gate.noteProgress()
            Thread.sleep(forTimeInterval: interval)
        }
    }

    // MARK: - Rule 1: exactly-once request

    func testSendRequestRunsOnceForConcurrentCallers() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let waiterCount = 4
        let exp = expectation(description: "concurrent waiters")
        exp.expectedFulfillmentCount = waiterCount
        let outcomes = OutcomeList()
        let ready = DispatchGroup()
        let go = DispatchSemaphore(value: 0)

        for _ in 0..<waiterCount {
            ready.enter()
            DispatchQueue.global(qos: .userInitiated).async {
                ready.leave()
                go.wait()
                outcomes.append(self.invokeEnsure(gate, semaphore: semaphore, probe: probe))
                exp.fulfill()
            }
        }

        if ready.wait(timeout: .now() + 2.0) == .timedOut {
            XCTFail("concurrent waiters did not reach the start barrier")
            for _ in 0..<waiterCount { go.signal() }
            cancelAndWait(gate, exp)
            return
        }
        for _ in 0..<waiterCount { go.signal() }

        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 }) else {
            cancelAndWait(gate, exp)
            return
        }
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }
        // Give the other callers time to enter as secondaries.
        Thread.sleep(forTimeInterval: 0.1)
        XCTAssertEqual(probe.sendCount, 1)

        semaphore.signal()
        wait(for: [exp], timeout: 2.0)

        XCTAssertEqual(probe.sendCount, 1)
        let all = outcomes.snapshot()
        XCTAssertEqual(all.count, waiterCount)
        for (index, outcome) in all.enumerated() {
            XCTAssertEqual(outcome, .completed(GateProbe.defaultPaths),
                           "waiter \(index) must see the published path list")
        }
    }

    func testSequentialCallsWhileWaitingStillSendOnlyOnce() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary")
        let secondaryExp = expectation(description: "secondary")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 1)
        XCTAssertEqual(probe.sendCountAtFirstIdle, Optional(1),
                       "sendRequest must run before the primary starts waiting")

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(gate.currentGeneration, 1, "a secondary joining must not advance generation")

        semaphore.signal()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)

        XCTAssertEqual(probe.sendCount, 1)
        assertOutcome(primaryOutcome, .completed(GateProbe.defaultPaths))
        assertOutcome(secondaryOutcome, .completed(GateProbe.defaultPaths))
    }

    // MARK: - Rule 2: completion by semaphore

    func testPrimaryCompletesWithParsedPathsOnceSemaphoreSignals() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(
            pollInterval: 0.05,
            payload: Data("/tmp/z.txt\n\n/tmp/a.txt\n/tmp/m.txt\n".utf8)
        )
        let expected = ["/tmp/z.txt", "/tmp/a.txt", "/tmp/m.txt"]
        let exp = expectation(description: "primary complete")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        semaphore.signal()
        wait(for: [exp], timeout: 2.0)

        assertOutcome(outcome, .completed(expected))
        guard let cached = gate.cachedPaths else {
            XCTFail("cachedPaths must equal the published list after completion")
            return
        }
        XCTAssertEqual(cached, expected)
        XCTAssertEqual(probe.payloadReads, 1)
        XCTAssertEqual(probe.events.first, Optional("sendRequest"))
    }

    func testCachedCallReturnsCompletedWithoutReenteringWait() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05)

        let first = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        XCTAssertEqual(first, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(gate.currentGeneration, 1)
        let idleAfterPrimary = probe.idleCount
        let payloadReadsAfterPrimary = probe.payloadReads
        XCTAssertEqual(payloadReadsAfterPrimary, 1)
        XCTAssertEqual(idleAfterPrimary, 0)

        let started = Date()
        let cached = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        let elapsed = Date().timeIntervalSince(started)

        XCTAssertEqual(cached, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(probe.idleCount, idleAfterPrimary)
        XCTAssertEqual(probe.payloadReads, payloadReadsAfterPrimary)
        XCTAssertEqual(gate.currentGeneration, 1, "a cached read must not advance generation")
        XCTAssertLessThan(elapsed, 0.2, "cached read must return immediately")

        let again = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        XCTAssertEqual(again, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(probe.events, ["sendRequest", "renderedPayload"])
    }

    // MARK: - Rule 3: secondary waiters converge

    func testSecondaryWaiterReturnsTheSameCompletedPathsWithoutSending() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary")
        let secondaryExp = expectation(description: "secondary")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }
        XCTAssertEqual(probe.sendCount, 1, "secondary must not send its own request")

        semaphore.signal()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)

        guard let primary = requireOutcome(primaryOutcome),
              let secondary = requireOutcome(secondaryOutcome) else { return }
        XCTAssertEqual(primary, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(secondary, primary)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
    }

    // MARK: - Rule 4: timeout is bounded

    func testPrimaryTimesOutNearTimeoutWhenNothingArrives() {
        let timeout: TimeInterval = 0.4
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary timeout")
        let outcome = OutcomeBox()

        let started = Date()
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        wait(for: [exp], timeout: 5.0)
        let elapsed = Date().timeIntervalSince(started)

        assertOutcome(outcome, .timedOut)
        XCTAssertGreaterThanOrEqual(elapsed, timeout * 0.5,
                                    "timeout must not fire well before the configured bound")
        XCTAssertLessThan(elapsed, timeout + 3.0,
                          "timeout must not hang unboundedly past the configured bound")
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered,
                       "timeout must re-arm the gate rather than poison the announcement")
        XCTAssertEqual(probe.payloadReads, 0)
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testSecondaryTimesOutNearTimeoutWhenNothingIsPublished() {
        let timeout: TimeInterval = 0.4
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary still waiting")
        let secondaryExp = expectation(description: "secondary timeout")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }

        let started = Date()
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        wait(for: [secondaryExp], timeout: 5.0)
        let elapsed = Date().timeIntervalSince(started)

        assertOutcome(secondaryOutcome, .timedOut)
        XCTAssertGreaterThanOrEqual(elapsed, timeout * 0.5,
                                    "secondary timeout must not fire well before the bound")
        XCTAssertLessThan(elapsed, timeout + 3.0,
                          "secondary timeout must not hang unboundedly past the bound")
        XCTAssertEqual(probe.sendCount, 1)

        wait(for: [primaryExp], timeout: 5.0)
        assertOutcome(primaryOutcome, .timedOut)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
    }

    // MARK: - Rule 5: cancel wins promptly

    func testCancelPlusSignalDuringPrimaryWaitYieldsCancelled() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary cancelled")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        let marked = Date()
        gate.cancel()
        semaphore.signal()
        wait(for: [exp], timeout: 2.0)
        let elapsed = Date().timeIntervalSince(marked)

        assertOutcome(outcome, .cancelled)
        XCTAssertLessThan(elapsed, 0.6, "cancel + signal must finish well before timeout")
        XCTAssertNil(gate.cachedPaths)
        XCTAssertTrue(gate.isCancelled)
        XCTAssertTrue(gate.hasTriggered, "cancel must not re-arm the gate")
        XCTAssertEqual(probe.payloadReads, 0)
    }

    func testCancelOnlyDuringPrimaryWaitYieldsCancelledPromptly() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary cancel-only")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        let marked = Date()
        gate.cancel()
        wait(for: [exp], timeout: 2.0)
        let elapsed = Date().timeIntervalSince(marked)

        assertOutcome(outcome, .cancelled)
        XCTAssertLessThan(elapsed, 0.6,
                          "cancel-only must return within a few poll intervals because the flag is polled")
        XCTAssertNil(gate.cachedPaths)
        XCTAssertTrue(gate.isCancelled)
        XCTAssertTrue(gate.hasTriggered, "cancel must not re-arm the gate")
        XCTAssertEqual(probe.payloadReads, 0)
    }

    func testSecondaryWaiterReturnsCancelled() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary cancel")
        let secondaryExp = expectation(description: "secondary cancel")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        gate.cancel()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)

        assertOutcome(primaryOutcome, .cancelled)
        assertOutcome(secondaryOutcome, .cancelled)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertTrue(gate.hasTriggered, "cancel must not re-arm the gate")
    }

    func testSemaphoreSignalAfterCancelIsNotCompletion() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "cancel then signal")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        gate.cancel()
        probe.setPayload(Data("/tmp/should-not-publish.txt".utf8))
        semaphore.signal()
        wait(for: [exp], timeout: 2.0)

        assertOutcome(outcome, .cancelled)
        XCTAssertNil(gate.cachedPaths, "a post-cancel signal must not publish paths")
        XCTAssertEqual(probe.payloadReads, 0)
    }

    func testIsCancelledReflectsFlagAndResetClearsIt() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        XCTAssertFalse(gate.isCancelled)
        XCTAssertEqual(gate.currentGeneration, 0)

        gate.cancel()
        XCTAssertTrue(gate.isCancelled)
        XCTAssertEqual(gate.currentGeneration, 0, "cancel() must not advance generation")

        gate.reset()
        XCTAssertFalse(gate.isCancelled)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(gate.currentGeneration, 1)
    }

    func testCancelBeforeEnsureDownloadedReturnsCancelledWithZeroIdle() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)

        gate.cancel()
        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        XCTAssertEqual(outcome, .cancelled)
        XCTAssertEqual(probe.idleCount, 0)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertTrue(gate.isCancelled)
        XCTAssertTrue(gate.hasTriggered, "cancel must not re-arm the gate")
    }

    // MARK: - Rule 6: supersede stops the wait

    func testPrimaryReturnsSupersededWhenAnnouncementStopsBeingCurrent() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary superseded")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        let marked = Date()
        probe.setStillCurrent(false)
        wait(for: [exp], timeout: 2.0)
        let elapsed = Date().timeIntervalSince(marked)

        assertOutcome(outcome, .superseded)
        XCTAssertLessThan(elapsed, 0.6)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertTrue(gate.hasTriggered, "supersede must not re-arm the gate")
        XCTAssertEqual(probe.payloadReads, 0)
    }

    func testSecondaryReturnsSupersededWhenAnnouncementStopsBeingCurrent() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary superseded")
        let secondaryExp = expectation(description: "secondary superseded")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        probe.setStillCurrent(false)
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)

        assertOutcome(primaryOutcome, .superseded)
        assertOutcome(secondaryOutcome, .superseded)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertTrue(gate.hasTriggered, "supersede must not re-arm the gate")
    }

    func testSignalledWaitFailsWhenAnnouncementAlreadyNotCurrent() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, stillCurrent: false)

        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        guard case .failed = outcome else {
            XCTFail("expected .failed when the semaphore fires for a non-current announcement, got \(outcome)")
            return
        }
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.idleCount, 0)
        XCTAssertFalse(gate.hasTriggered, ".failed re-arms the gate")
    }

    // MARK: - Rule 7: payload validation

    func testNilRenderedPayloadFailsAndDoesNotCache() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, payload: nil)

        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        guard case .failed = outcome else {
            XCTFail("expected .failed when PROVIDE_DATA is signalled with a nil payload, got \(outcome)")
            return
        }
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.payloadReads, 1)
        XCTAssertFalse(gate.hasTriggered, ".failed re-arms the gate")
    }

    func testNonUTF8RenderedPayloadFailsAndDoesNotCache() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, payload: Data([0xFF, 0xFE]))

        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        guard case .failed = outcome else {
            XCTFail("expected .failed when PROVIDE_DATA bytes are not UTF-8, got \(outcome)")
            return
        }
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered, ".failed re-arms the gate")
    }

    func testParsePathListEmptyPayloadYieldsEmptyArray() {
        guard let parsed = FileDownloadGate.parsePathList(Data()) else {
            XCTFail("empty UTF-8 payload must parse as an empty list, not nil")
            return
        }
        XCTAssertEqual(parsed, [])
    }

    func testParsePathListDropsBlankLinesAndTrailingNewlinePreservingOrder() {
        guard let parsed = FileDownloadGate.parsePathList(
            Data("/tmp/z.txt\n\n/tmp/a.txt\n/tmp/m.txt\n".utf8)
        ) else {
            XCTFail("valid UTF-8 path list must parse")
            return
        }
        XCTAssertEqual(parsed, ["/tmp/z.txt", "/tmp/a.txt", "/tmp/m.txt"])
    }

    func testParsePathListDoesNotStripCarriageReturn() {
        // components(separatedBy: "\n") leaves a trailing "\r" on each
        // CRLF-delimited line. That is the documented parse contract.
        guard let crlf = FileDownloadGate.parsePathList(Data("one\r\ntwo".utf8)) else {
            XCTFail("CRLF payload is still valid UTF-8")
            return
        }
        XCTAssertEqual(crlf, ["one\r", "two"])

        guard let trailing = FileDownloadGate.parsePathList(Data("one\r\ntwo\r\n".utf8)) else {
            XCTFail("CRLF payload with trailing newline is still valid UTF-8")
            return
        }
        XCTAssertEqual(trailing, ["one\r", "two\r"])
    }

    func testParsePathListRejectsNonUTF8() {
        XCTAssertNil(FileDownloadGate.parsePathList(Data([0xFF, 0xFE])))
    }

    // MARK: - Rule 8: idle is the wait point

    func testIdleIsCalledDuringAWaitSpanningSeveralPollIntervals() {
        let timeout: TimeInterval = 0.35
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "idle during timeout wait")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        wait(for: [exp], timeout: 5.0)

        assertOutcome(outcome, .timedOut)
        XCTAssertGreaterThanOrEqual(probe.idleCount, 1,
                                    "idle must be invoked while waiting with nothing to do")
    }

    func testIdleIsNotCalledWhenPathsAlreadyCached() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05)

        XCTAssertEqual(invokeEnsure(gate, semaphore: semaphore, probe: probe),
                       .completed(GateProbe.defaultPaths))

        let idleBefore = probe.idleCount
        XCTAssertEqual(invokeEnsure(gate, semaphore: semaphore, probe: probe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.idleCount, idleBefore)
    }

    func testIdleIsNotCalledWhenAlreadyCancelledOnEntry() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary")
        let primaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        gate.cancel()
        wait(for: [primaryExp], timeout: 2.0)
        assertOutcome(primaryOutcome, .cancelled)
        XCTAssertTrue(gate.hasTriggered, "cancel must not re-arm the gate")

        let lateProbe = GateProbe(pollInterval: 0.05)
        let late = invokeEnsure(gate, semaphore: semaphore, probe: lateProbe)
        XCTAssertEqual(late, .cancelled)
        XCTAssertEqual(lateProbe.idleCount, 0)
        XCTAssertEqual(lateProbe.sendCount, 0)
        XCTAssertTrue(gate.hasTriggered)
    }

    // MARK: - Rule 9: reset isolates announcement state

    func testResetAfterCompletionStartsANewPrimary() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let firstSemaphore = DispatchSemaphore(value: 0)
        firstSemaphore.signal()
        let firstProbe = GateProbe(pollInterval: 0.05)

        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 0)
        XCTAssertEqual(invokeEnsure(gate, semaphore: firstSemaphore, probe: firstProbe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 1)

        gate.reset()
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertFalse(gate.isCancelled)
        XCTAssertEqual(gate.currentGeneration, 2)

        let secondSemaphore = DispatchSemaphore(value: 0)
        secondSemaphore.signal()
        let secondProbe = GateProbe(
            pollInterval: 0.05,
            payload: Data("/tmp/next.txt".utf8)
        )
        XCTAssertEqual(invokeEnsure(gate, semaphore: secondSemaphore, probe: secondProbe),
                       .completed(["/tmp/next.txt"]))
        XCTAssertEqual(secondProbe.sendCount, 1)
        XCTAssertEqual(gate.cachedPaths, Optional(["/tmp/next.txt"]))
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 3)
    }

    func testResetAfterTimeoutStartsANewPrimary() {
        let timeout: TimeInterval = 0.3
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let firstSemaphore = DispatchSemaphore(value: 0)
        let firstProbe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "timeout then reset")
        let firstOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: firstSemaphore, probe: firstProbe,
                    expectation: exp, outcome: firstOutcome)
        wait(for: [exp], timeout: 5.0)
        assertOutcome(firstOutcome, .timedOut)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(firstProbe.sendCount, 1)
        XCTAssertFalse(gate.hasTriggered, "timeout re-arms even before reset()")

        gate.reset()
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertFalse(gate.isCancelled)

        let secondSemaphore = DispatchSemaphore(value: 0)
        secondSemaphore.signal()
        let secondProbe = GateProbe(pollInterval: 0.05)
        XCTAssertEqual(invokeEnsure(gate, semaphore: secondSemaphore, probe: secondProbe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(secondProbe.sendCount, 1)
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
        XCTAssertTrue(gate.hasTriggered)
    }

    // MARK: - Rule 10: empty PROVIDE_DATA is a failure, not a cached success

    func testEmptyProvideDataFailsAndDoesNotCache() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, payload: Data())

        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        guard case .failed = outcome else {
            XCTFail("expected .failed when PROVIDE_DATA is empty, got \(outcome)")
            return
        }
        XCTAssertNil(gate.cachedPaths, "empty PROVIDE_DATA must not be cached as a path list")
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(probe.payloadReads, 1)
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testNewlinesOnlyProvideDataFailsAndDoesNotCache() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, payload: Data("\n\n\n".utf8))

        let outcome = invokeEnsure(gate, semaphore: semaphore, probe: probe)

        guard case .failed = outcome else {
            XCTFail("expected .failed when PROVIDE_DATA is only newlines, got \(outcome)")
            return
        }
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testEmptyProvideDataIsNotPublishedToSecondaryAsCompleted() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05, payload: Data())
        let primaryExp = expectation(description: "primary empty payload")
        let secondaryExp = expectation(description: "secondary empty payload")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        let marked = Date()
        semaphore.signal()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)
        let elapsed = Date().timeIntervalSince(marked)

        guard let primary = requireOutcome(primaryOutcome),
              let secondary = requireOutcome(secondaryOutcome) else { return }
        guard case .failed = primary else {
            XCTFail("expected primary .failed for empty PROVIDE_DATA, got \(primary)")
            return
        }
        XCTAssertEqual(secondary, primary,
                       "secondary must see the same failure, not .completed([])")
        XCTAssertLessThan(elapsed, 0.6, "secondary must inherit the failure promptly")
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testParsePathListNewlinesOnlyYieldsEmptyArray() {
        guard let parsed = FileDownloadGate.parsePathList(Data("\n\n".utf8)) else {
            XCTFail("newlines-only UTF-8 payload must parse as an empty list, not nil")
            return
        }
        XCTAssertEqual(parsed, [])
    }

    // MARK: - Rule 11: timeout is a stall bound refreshed by noteProgress()

    func testProgressShorterThanStallTimeoutKeepsPrimaryWaiting() {
        let timeout: TimeInterval = 0.3
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary kept alive by progress")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        pulseProgress(gate, every: 0.1, for: 0.8, watching: [outcome])
        XCTAssertNil(outcome.get(),
                     "primary must still be waiting at ~0.8s when progress arrives every 0.1s")
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertEqual(probe.sendCount, 1)

        semaphore.signal()
        wait(for: [exp], timeout: 2.0)

        assertOutcome(outcome, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testTimesOutFromLastProgressNotFromWaitStart() {
        let timeout: TimeInterval = 0.4
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "stall after last progress")
        let outcome = OutcomeBox()

        let waitStarted = Date()
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        Thread.sleep(forTimeInterval: 0.2)
        XCTAssertNil(outcome.get(), "must still be waiting before the first progress mark")
        gate.noteProgress()
        let marked = Date()

        wait(for: [exp], timeout: 5.0)
        let fromProgress = Date().timeIntervalSince(marked)
        let fromStart = Date().timeIntervalSince(waitStarted)

        assertOutcome(outcome, .timedOut)
        XCTAssertGreaterThanOrEqual(fromProgress, timeout * 0.5,
                                    "stall must be measured from the last noteProgress()")
        XCTAssertLessThan(fromProgress, timeout + 3.0)
        XCTAssertGreaterThan(fromStart, timeout,
                             "wait must outlive the original stall bound when progress arrived")
        XCTAssertNil(gate.cachedPaths)
        XCTAssertFalse(gate.hasTriggered)
    }

    func testProgressRecordedBeforeWaitDoesNotExtendIt() {
        let timeout: TimeInterval = 0.3
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "stale progress does not extend")
        let outcome = OutcomeBox()

        gate.noteProgress()
        Thread.sleep(forTimeInterval: 0.22)

        let started = Date()
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        wait(for: [exp], timeout: 5.0)
        let elapsed = Date().timeIntervalSince(started)

        assertOutcome(outcome, .timedOut)
        XCTAssertGreaterThanOrEqual(elapsed, timeout * 0.5,
                                    "a progress mark from before the wait must not shrink or extend it")
        XCTAssertLessThan(elapsed, timeout + 3.0)
        XCTAssertFalse(gate.hasTriggered)
    }

    func testProgressExtendsSecondaryWaitTheSameWay() {
        let timeout: TimeInterval = 0.3
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary kept alive")
        let secondaryExp = expectation(description: "secondary kept alive")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        pulseProgress(gate, every: 0.1, for: 0.8, watching: [primaryOutcome, secondaryOutcome])
        XCTAssertNil(primaryOutcome.get(), "primary must still be waiting while progress flows")
        XCTAssertNil(secondaryOutcome.get(), "secondary must be extended by progress the same way")
        XCTAssertEqual(probe.sendCount, 1)

        semaphore.signal()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)

        assertOutcome(primaryOutcome, .completed(GateProbe.defaultPaths))
        assertOutcome(secondaryOutcome, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 1)
    }

    func testResetDuringPrimaryWaitReturnsSupersededPromptly() {
        let timeout: TimeInterval = 2.0
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary superseded by reset")
        let outcome = OutcomeBox()

        let started = Date()
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 1)

        // Progress would push the stall deadline to now+timeout. reset()
        // must abort the wait as `.superseded` instead of letting that
        // extension (or the original stall bound) elapse.
        gate.noteProgress()
        let marked = Date()
        gate.reset()
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(gate.currentGeneration, 2)

        wait(for: [exp], timeout: 5.0)
        let afterReset = Date().timeIntervalSince(marked)
        let fromStart = Date().timeIntervalSince(started)

        assertOutcome(outcome, .superseded)
        XCTAssertLessThan(afterReset, 0.6,
                          "reset must make the primary return .superseded within a few poll intervals")
        XCTAssertLessThan(fromStart, timeout,
                          "must not wait out the stall bound or the post-progress extension")
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(probe.payloadReads, 0)
        XCTAssertEqual(gate.currentGeneration, 2)
    }

    // MARK: - Rule 12: giving up re-arms; cancel/supersede do not

    func testTimedOutPrimaryRearmsSoNextCallerSendsAgain() {
        let timeout: TimeInterval = 0.3
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary timeout then retry")
        let firstOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: firstOutcome)
        wait(for: [exp], timeout: 5.0)
        assertOutcome(firstOutcome, .timedOut)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(gate.currentGeneration, 1, "timing out must not advance generation")

        semaphore.signal()
        let second = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        XCTAssertEqual(second, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 2, "re-armed caller must send a fresh DATA_REQUEST")
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 2)
    }

    func testFailedPrimaryRearmsSoNextCallerSendsAgain() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        semaphore.signal()
        let probe = GateProbe(pollInterval: 0.05, payload: Data())

        let first = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        guard case .failed = first else {
            XCTFail("expected .failed for empty PROVIDE_DATA, got \(first)")
            return
        }
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)

        probe.setPayload(GateProbe.defaultPayload)
        semaphore.signal()
        let second = invokeEnsure(gate, semaphore: semaphore, probe: probe)
        XCTAssertEqual(second, .completed(GateProbe.defaultPaths))
        XCTAssertEqual(probe.sendCount, 2, "re-armed caller must send a fresh DATA_REQUEST")
        XCTAssertEqual(gate.cachedPaths, Optional(GateProbe.defaultPaths))
        XCTAssertTrue(gate.hasTriggered)
    }

    func testSecondaryInheritsTimedOutPromptlyWhenPrimaryTimesOut() {
        let timeout: TimeInterval = 1.0
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary stall")
        let secondaryExp = expectation(description: "secondary inherits stall")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }

        // Start the secondary late so its own stall bound would still have
        // ~timeout remaining after the primary gives up. Inheriting promptly
        // is what distinguishes re-arm from "wait your own full timeout".
        Thread.sleep(forTimeInterval: 0.5)
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 && probe.sendCount == 1 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        wait(for: [primaryExp], timeout: 5.0)
        let afterPrimary = Date()
        wait(for: [secondaryExp], timeout: 5.0)
        let inheritElapsed = Date().timeIntervalSince(afterPrimary)

        assertOutcome(primaryOutcome, .timedOut)
        assertOutcome(secondaryOutcome, .timedOut)
        XCTAssertLessThan(inheritElapsed, 0.35,
                          "secondary must inherit .timedOut within a couple of poll intervals, not after its own stall bound")
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
    }

    func testSecondaryInheritsFailedPromptlyWhenPrimaryFails() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05, payload: Data([0xFF, 0xFE]))
        let primaryExp = expectation(description: "primary fail")
        let secondaryExp = expectation(description: "secondary inherits fail")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }

        let marked = Date()
        semaphore.signal()
        wait(for: [primaryExp, secondaryExp], timeout: 2.0)
        let elapsed = Date().timeIntervalSince(marked)

        guard let primary = requireOutcome(primaryOutcome),
              let secondary = requireOutcome(secondaryOutcome) else { return }
        guard case .failed = primary else {
            XCTFail("expected primary .failed, got \(primary)")
            return
        }
        XCTAssertEqual(secondary, primary,
                       "secondary must return the same .failed(_) the primary returned")
        XCTAssertLessThan(elapsed, 0.6,
                          "secondary must inherit .failed promptly, not after its own stall bound")
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
    }

    func testSupersedeDoesNotRearmHasTriggeredStaysTrue() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary superseded")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        probe.setStillCurrent(false)
        wait(for: [exp], timeout: 2.0)
        assertOutcome(outcome, .superseded)
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)

        let lateProbe = GateProbe(pollInterval: 0.05, stillCurrent: false)
        let late = invokeEnsure(gate, semaphore: semaphore, probe: lateProbe)
        XCTAssertEqual(late, .superseded)
        XCTAssertEqual(lateProbe.sendCount, 0, "a post-supersede caller must not send a new request")
        XCTAssertEqual(lateProbe.idleCount, 0)
        XCTAssertTrue(gate.hasTriggered)
    }

    func testCancelDoesNotRearmLaterCallersStayCancelled() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary cancelled")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        gate.cancel()
        wait(for: [exp], timeout: 2.0)
        assertOutcome(outcome, .cancelled)
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertTrue(gate.isCancelled)

        let lateProbe = GateProbe(pollInterval: 0.05)
        let late = invokeEnsure(gate, semaphore: semaphore, probe: lateProbe)
        XCTAssertEqual(late, .cancelled)
        XCTAssertEqual(lateProbe.sendCount, 0)
        XCTAssertEqual(lateProbe.idleCount, 0)
        XCTAssertTrue(gate.hasTriggered)

        gate.reset()
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertFalse(gate.isCancelled)

        let retrySemaphore = DispatchSemaphore(value: 0)
        retrySemaphore.signal()
        let retryProbe = GateProbe(pollInterval: 0.05)
        XCTAssertEqual(invokeEnsure(gate, semaphore: retrySemaphore, probe: retryProbe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(retryProbe.sendCount, 1)
    }

    // MARK: - Rule 13: currentGeneration advances only on primary start and reset

    func testCurrentGenerationAdvancesOnlyOnPrimaryStartAndReset() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        XCTAssertEqual(gate.currentGeneration, 0)

        gate.noteProgress()
        XCTAssertEqual(gate.currentGeneration, 0, "noteProgress() must not advance generation")
        gate.cancel()
        XCTAssertEqual(gate.currentGeneration, 0, "cancel() must not advance generation")
        gate.reset()
        XCTAssertEqual(gate.currentGeneration, 1)
        XCTAssertFalse(gate.isCancelled)

        let ready = DispatchSemaphore(value: 0)
        ready.signal()
        let probe = GateProbe(pollInterval: 0.05)
        XCTAssertEqual(invokeEnsure(gate, semaphore: ready, probe: probe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(gate.currentGeneration, 2, "becoming primary must advance generation by one")
        XCTAssertEqual(probe.sendCount, 1)

        XCTAssertEqual(invokeEnsure(gate, semaphore: ready, probe: probe),
                       .completed(GateProbe.defaultPaths))
        XCTAssertEqual(gate.currentGeneration, 2, "a cached read must not advance generation")
        XCTAssertEqual(probe.sendCount, 1)

        gate.reset()
        XCTAssertEqual(gate.currentGeneration, 3)

        let waitSemaphore = DispatchSemaphore(value: 0)
        let waitProbe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "generation primary")
        let secondaryExp = expectation(description: "generation secondary")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: waitSemaphore, probe: waitProbe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { waitProbe.sendCount == 1 && waitProbe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 4)

        startWaiter(gate: gate, semaphore: waitSemaphore, probe: waitProbe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { waitProbe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 4, "a secondary joining must not advance generation")
        XCTAssertEqual(waitProbe.sendCount, 1)

        cancelAndWait(gate, primaryExp, secondaryExp)
        XCTAssertEqual(gate.currentGeneration, 4, "cancel() of in-flight waiters must not advance generation")
    }

    // MARK: - Rule 14: reset during primary wait does not publish late data

    func testResetDuringPrimaryWaitDoesNotPublishLateSignal() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let exp = expectation(description: "primary superseded, late signal ignored")
        let outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: exp, outcome: outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 1 }) else {
            cancelAndWait(gate, exp)
            return
        }

        probe.setPayload(Data("/tmp/late.txt".utf8))
        gate.reset()
        semaphore.signal()
        wait(for: [exp], timeout: 5.0)

        assertOutcome(outcome, .superseded)
        XCTAssertNil(gate.cachedPaths, "a post-reset signal must not publish the old attempt's payload")
        XCTAssertEqual(probe.payloadReads, 0)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 2)

        let nextSemaphore = DispatchSemaphore(value: 0)
        nextSemaphore.signal()
        let nextProbe = GateProbe(pollInterval: 0.05, payload: Data("/tmp/fresh.txt".utf8))
        XCTAssertEqual(invokeEnsure(gate, semaphore: nextSemaphore, probe: nextProbe),
                       .completed(["/tmp/fresh.txt"]))
        XCTAssertEqual(nextProbe.sendCount, 1)
        XCTAssertEqual(gate.cachedPaths, Optional(["/tmp/fresh.txt"]))
        XCTAssertEqual(gate.currentGeneration, 3)
    }

    // MARK: - Rule 15: reset during secondary wait

    func testResetDuringSecondaryWaitReturnsSupersededPromptly() {
        let gate = FileDownloadGate(timeout: 2.0, pollInterval: 0.05)
        let semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05)
        let primaryExp = expectation(description: "primary reset-superseded")
        let secondaryExp = expectation(description: "secondary reset-superseded")
        let primaryOutcome = OutcomeBox()
        let secondaryOutcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: primaryExp, outcome: primaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            cancelAndWait(gate, primaryExp)
            return
        }
        startWaiter(gate: gate, semaphore: semaphore, probe: probe,
                    expectation: secondaryExp, outcome: secondaryOutcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.idleCount >= 2 }) else {
            cancelAndWait(gate, primaryExp, secondaryExp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 1)

        let marked = Date()
        gate.reset()
        wait(for: [primaryExp, secondaryExp], timeout: 5.0)
        let elapsed = Date().timeIntervalSince(marked)

        assertOutcome(primaryOutcome, .superseded)
        assertOutcome(secondaryOutcome, .superseded)
        XCTAssertLessThan(elapsed, 0.6,
                          "reset must make the secondary return .superseded within a couple of poll intervals")
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertNil(gate.cachedPaths)
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(probe.payloadReads, 0)
        XCTAssertEqual(gate.currentGeneration, 2)
    }

    // MARK: - Rule 16: ABA — stale secondary must not report an old failure into a new attempt

    func testSecondaryIsSupersededWhenNewerPrimaryStartsAfterItsPrimaryGaveUp() {
        let timeout: TimeInterval = 0.4
        let gate = FileDownloadGate(timeout: timeout, pollInterval: 0.05)
        let p1Semaphore = DispatchSemaphore(value: 0)
        let p2Semaphore = DispatchSemaphore(value: 0)
        let probe = GateProbe(pollInterval: 0.05, payload: Data("/tmp/p2.txt".utf8))
        let park = IdlePark()
        let s1Probe = GateProbe(pollInterval: 0.05)
        s1Probe.setIdleOverride { park.park() }

        let p1Exp = expectation(description: "P1 timeout")
        let s1Exp = expectation(description: "S1 superseded")
        let p2Exp = expectation(description: "P2 complete")
        let p1Outcome = OutcomeBox()
        let s1Outcome = OutcomeBox()
        let p2Outcome = OutcomeBox()

        startWaiter(gate: gate, semaphore: p1Semaphore, probe: probe,
                    expectation: p1Exp, outcome: p1Outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 1 && probe.idleCount >= 1 }) else {
            park.release()
            cancelAndWait(gate, p1Exp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 1)

        startWaiter(gate: gate, semaphore: p1Semaphore, probe: s1Probe,
                    expectation: s1Exp, outcome: s1Outcome)
        guard waitUntil(timeout: 1.0, predicate: { s1Probe.idleCount >= 1 }) else {
            park.release()
            cancelAndWait(gate, p1Exp, s1Exp)
            return
        }
        XCTAssertEqual(s1Probe.sendCount, 0, "S1 must have joined as a secondary")
        XCTAssertEqual(probe.sendCount, 1)
        XCTAssertEqual(gate.currentGeneration, 1)
        XCTAssertTrue(gate.hasTriggered)

        wait(for: [p1Exp], timeout: 5.0)
        assertOutcome(p1Outcome, .timedOut)
        XCTAssertFalse(gate.hasTriggered)
        XCTAssertEqual(gate.currentGeneration, 1, "P1 giving up must not advance generation")
        XCTAssertNil(s1Outcome.get(), "S1 must still be frozen across P1's timeout")

        // Refresh S1's stall bound so waking it cannot return `.timedOut`
        // from `hasStalled` before it observes the generation change.
        // `noteProgress` must not itself advance generation (rule 13).
        gate.noteProgress()
        XCTAssertEqual(gate.currentGeneration, 1)

        startWaiter(gate: gate, semaphore: p2Semaphore, probe: probe,
                    expectation: p2Exp, outcome: p2Outcome)
        guard waitUntil(timeout: 1.0, predicate: { probe.sendCount == 2 }) else {
            park.release()
            cancelAndWait(gate, s1Exp, p2Exp)
            return
        }
        XCTAssertEqual(gate.currentGeneration, 2)
        XCTAssertTrue(gate.hasTriggered)
        XCTAssertNil(s1Outcome.get(), "S1 must still be frozen until released")
        XCTAssertEqual(s1Probe.sendCount, 0)

        park.release()
        wait(for: [s1Exp], timeout: 5.0)

        guard let s1 = requireOutcome(s1Outcome) else {
            park.release()
            cancelAndWait(gate, p2Exp)
            return
        }
        XCTAssertEqual(s1, .superseded,
                       "S1 must not inherit P1's .timedOut or P2's eventual result")
        XCTAssertEqual(s1Probe.sendCount, 0, "S1 must not send a request")
        XCTAssertNil(p2Outcome.get(), "P2 must still be waiting after S1 returns")

        p2Semaphore.signal()
        wait(for: [p2Exp], timeout: 2.0)

        assertOutcome(p2Outcome, .completed(["/tmp/p2.txt"]))
        XCTAssertEqual(probe.sendCount, 2)
        XCTAssertEqual(s1Probe.sendCount, 0)
        XCTAssertEqual(gate.cachedPaths, Optional(["/tmp/p2.txt"]))
        XCTAssertEqual(gate.currentGeneration, 2)
        XCTAssertTrue(gate.hasTriggered)
    }
}

// MARK: - Test doubles

private final class GateProbe {
    static let defaultPayload = Data("/tmp/a.txt\n/tmp/b.txt".utf8)
    static let defaultPaths = ["/tmp/a.txt", "/tmp/b.txt"]

    let pollInterval: TimeInterval
    private let lock = NSLock()
    private var sendCountValue = 0
    private var idleCountValue = 0
    private var payloadReadsValue = 0
    private var sendCountAtFirstIdleValue: Int?
    private var stillCurrentValue = true
    private var payloadValue: Data?
    private var eventsValue: [String] = []
    private var idleOverride: (() -> Void)?

    init(pollInterval: TimeInterval,
         payload: Data? = GateProbe.defaultPayload,
         stillCurrent: Bool = true) {
        self.pollInterval = pollInterval
        self.payloadValue = payload
        self.stillCurrentValue = stillCurrent
    }

    var sendCount: Int { locked { sendCountValue } }
    var idleCount: Int { locked { idleCountValue } }
    var payloadReads: Int { locked { payloadReadsValue } }
    var sendCountAtFirstIdle: Int? { locked { sendCountAtFirstIdleValue } }
    var events: [String] { locked { eventsValue } }

    func setStillCurrent(_ value: Bool) {
        locked { stillCurrentValue = value }
    }

    func setPayload(_ value: Data?) {
        locked { payloadValue = value }
    }

    func setIdleOverride(_ override: (() -> Void)?) {
        locked { idleOverride = override }
    }

    func sendRequest() {
        locked {
            sendCountValue += 1
            eventsValue.append("sendRequest")
        }
    }

    func isStillCurrent() -> Bool {
        locked { stillCurrentValue }
    }

    func renderedPayload() -> Data? {
        locked {
            payloadReadsValue += 1
            eventsValue.append("renderedPayload")
            return payloadValue
        }
    }

    func idle() {
        let override: (() -> Void)?
        lock.lock()
        idleCountValue += 1
        if sendCountAtFirstIdleValue == nil {
            sendCountAtFirstIdleValue = sendCountValue
        }
        eventsValue.append("idle")
        override = idleOverride
        lock.unlock()
        if let override {
            override()
        } else {
            Thread.sleep(forTimeInterval: pollInterval)
        }
    }

    @discardableResult
    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

private final class OutcomeBox {
    private let lock = NSLock()
    private var value: FileDownloadWaitOutcome?

    func set(_ newValue: FileDownloadWaitOutcome) {
        lock.lock()
        value = newValue
        lock.unlock()
    }

    func get() -> FileDownloadWaitOutcome? {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

private final class OutcomeList {
    private let lock = NSLock()
    private var items: [FileDownloadWaitOutcome] = []

    func append(_ item: FileDownloadWaitOutcome) {
        lock.lock()
        items.append(item)
        lock.unlock()
    }

    func snapshot() -> [FileDownloadWaitOutcome] {
        lock.lock()
        defer { lock.unlock() }
        return items
    }
}

/// Blocks in `park()` until `release()`. Freezes a secondary between
/// polls so the test thread can let P1 give up and start P2 first.
private final class IdlePark {
    private let condition = NSCondition()
    private var released = false

    func park() {
        condition.lock()
        while !released {
            condition.wait()
        }
        condition.unlock()
    }

    func release() {
        condition.lock()
        released = true
        condition.broadcast()
        condition.unlock()
    }
}
