// chunk_provider_test.cpp — contract tests for DispatcherChunkProvider, the
// FILE_CHUNK_REQUEST / FILE_CHUNK_DATA round-trip a virtual-file paste runs
// on the STA thread.
//
// These pin the CONTRACT documented in chunk_provider.h and the ChunkProvider
// base in virtual_file_provider.h, not the current shape of the wait loop.
// FetchChunk blocks, so every case that has to observe a blocked call runs it
// on a WaiterThread and drives DeliverChunk / CancelAll from the test thread.
// Every wait here is bounded and every waiter thread is cancelled + joined by
// its destructor, so a broken contract shows up as a red test, never a hang.
//
// Rules pinned:
//   1  Construction — timeout_ms() echoes the constructor argument, the
//      default is kDefaultChunkTimeoutMs (30 000), PendingCount() starts 0.
//   2  No transport — an empty SendFrameFn returns E_FAIL immediately and
//      stages nothing.
//   3  Argument validation — empty transfer_id or file_id is E_INVALIDARG
//      with nothing sent; offset == 0 and size == 0 are legal values.
//   4  Send failure — SendFrameFn returning false is E_FAIL well before the
//      timeout, with the staged slot removed.
//   5  Request frame — exactly one frame per call, decoding to a
//      FILE_CHUNK_REQUEST carrying the four arguments, sent BEFORE the call
//      blocks (observed: the slot is already staged during the send).
//   6  Happy path — matching DeliverChunk yields S_OK, the exact bytes and
//      is_last; an empty payload with no error is still S_OK.
//   7  Correlation — a delivery for a different transfer_id / file_id /
//      offset does not wake a waiter; a delivery nobody waits on is a no-op.
//   8  Parent error — a non-empty error string is E_FAIL, promptly, with
//      out_data empty and out_is_last false.
//   9  Timeout — no delivery means E_FAIL at roughly the timeout, no slot
//      left behind, and a late delivery is a no-op.
//  10  Cancellation — CancelAll wakes in-flight waiters with STG_E_REVERTED
//      promptly; it is a no-op when idle and it is NOT sticky.
//  11  Multiple waiters — distinct tuples are independent; CancelAll wakes
//      every one of them.
//  12  Same-key FIFO — the OLDER waiter on a tuple is completed first, and
//      neither of two same-key waiters is stranded.
//  13  Message pumping — window messages posted to the waiting thread are
//      dispatched while FetchChunk waits.
//  14  WM_QUIT — aborts the wait with E_ABORT and is re-posted so the outer
//      loop still sees it.
//  15  Reuse — the same provider serves the same tuple again after success,
//      a parent error, a timeout and a cancellation.
//  16  Back-to-back replies — two replies for the same tuple, delivered with
//      no interlock between them, complete two waiters in FIFO order and
//      strand neither. A reply CONSUMES its request, so PendingCount() is
//      already 0 when the second DeliverChunk returns.
//  17  PendingCount() drops when the reply lands, not when the waiter wakes:
//      observed with a waiter provably parked inside FetchChunk's own message
//      pump, so it cannot be the waiter that removed the slot.
//
// NOT pinned on purpose:
//   * The race where DeliverChunk and CancelAll both land before the waiter
//     wakes. Either outcome (S_OK or STG_E_REVERTED) is defensible and any
//     test of it would be timing-dependent, so it is left unspecified rather
//     than frozen by a flaky test.
//   * The HANDLE-lifetime race — SetEvent on an event whose waiter had
//     already closed it. The window is sub-microsecond and there is no
//     deterministic way to sit inside it from a test; the guarantee is
//     structural (signal and close happen under the same lock), not
//     observable through the public surface.
//   * The relative precedence of the "no transport" and "empty id" checks
//     when BOTH are wrong (the implementation answers E_FAIL; the contract
//     does not say).

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chunk_provider.h"
#include "dispatch_codec.h"
#include "helper_proto.h"

namespace codec = leviathan::clipboard_helper::dispatch_codec;
namespace proto = leviathan::clipboard_helper::proto;

using leviathan::clipboard_helper::DispatcherChunkProvider;
using leviathan::clipboard_helper::kDefaultChunkTimeoutMs;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Ms    = std::chrono::milliseconds;

// Budget for every bounded poll. Generous enough that a loaded machine never
// trips it, short enough that a broken contract fails in seconds.
constexpr Ms kPollBudget{5000};

// How long "still pending" observations wait before concluding nothing woke.
constexpr Ms kGrace{200};

// A "prompt" outcome (send failure, parent error, cancellation) must beat
// this; providers used for those cases carry kLongTimeoutMs so the assertion
// really distinguishes prompt from timed-out.
constexpr long long kPromptMs       = 1500;
constexpr DWORD     kLongTimeoutMs  = 5000;
constexpr DWORD     kShortTimeoutMs = 500;

// Poll `pred` every millisecond until it holds or `budget` expires.
template <typename Pred>
bool PollUntil(Pred pred, Ms budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(Ms(1));
    }
}

bool WaitForPending(const DispatcherChunkProvider& provider, std::size_t expected) {
    return PollUntil([&provider, expected]() { return provider.PendingCount() == expected; },
                     kPollBudget);
}

// ─── Harness: a provider plus the frames its SendFrameFn saw ──────────────
//
// Records every frame (so rules 3 and 5 can decode one), remembers the
// provider's PendingCount() at the moment of the send (so rule 5 can prove
// the slot is staged before the call blocks), and can be told to fail the
// send (rule 4).
class Harness {
public:
    explicit Harness(DWORD timeout_ms, bool send_ok = true) : send_ok_(send_ok) {
        provider_ = std::make_unique<DispatcherChunkProvider>(
            [this](const Bytes& frame) { return this->OnSend(frame); }, timeout_ms);
    }

    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    DispatcherChunkProvider& provider() { return *provider_; }

    void set_send_ok(bool ok) { send_ok_.store(ok, std::memory_order_release); }

    std::size_t frame_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_.size();
    }

    Bytes frame(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (index >= frames_.size()) return Bytes{};
        return frames_[index];
    }

    // PendingCount() sampled inside the SendFrameFn of the most recent send.
    std::size_t pending_during_last_send() const {
        return pending_during_send_.load(std::memory_order_acquire);
    }

private:
    bool OnSend(const Bytes& frame) {
        pending_during_send_.store(provider_->PendingCount(), std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mu_);
            frames_.push_back(frame);
        }
        return send_ok_.load(std::memory_order_acquire);
    }

    mutable std::mutex       mu_;
    std::vector<Bytes>       frames_;
    std::atomic<bool>        send_ok_;
    std::atomic<std::size_t> pending_during_send_{0};
    // Declared last so it is destroyed first — the SendFrameFn closure holds
    // `this` and must not outlive the members it touches.
    std::unique_ptr<DispatcherChunkProvider> provider_;
};

struct FetchOutcome {
    HRESULT   hr{E_UNEXPECTED};
    Bytes     data;
    bool      is_last{false};
    long long elapsed_ms{0};
};

// ─── WaiterThread: one blocked FetchChunk on its own thread ───────────────
//
// `before` runs on the waiter thread before FetchChunk (used to create the
// thread's message queue / a message-only window); `after` runs on it once
// FetchChunk has returned (window teardown, WM_QUIT drain). The destructor
// cancels and joins on every path, so an ASSERT_* that returns early from a
// test still leaves no thread behind.
class WaiterThread {
public:
    WaiterThread(DispatcherChunkProvider& provider,
                 std::string              transfer_id,
                 std::string              file_id,
                 std::uint64_t            offset,
                 std::uint32_t            size,
                 std::function<void()>    before = nullptr,
                 std::function<void()>    after  = nullptr)
        : provider_(provider), before_(std::move(before)), after_(std::move(after)) {
        std::promise<FetchOutcome> promise;
        future_ = promise.get_future();
        thread_ = std::thread(
            [this, p = std::move(promise), transfer_id, file_id, offset, size]() mutable {
                tid_.store(::GetCurrentThreadId(), std::memory_order_release);
                if (before_) before_();
                ready_.store(true, std::memory_order_release);

                FetchOutcome outcome;
                const auto   start = std::chrono::steady_clock::now();
                outcome.hr = provider_.FetchChunk(transfer_id, file_id, offset, size,
                                                  outcome.data, outcome.is_last);
                outcome.elapsed_ms = std::chrono::duration_cast<Ms>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                if (after_) after_();
                p.set_value(std::move(outcome));
            });
    }

    WaiterThread(const WaiterThread&)            = delete;
    WaiterThread& operator=(const WaiterThread&) = delete;

    ~WaiterThread() {
        provider_.CancelAll();
        if (thread_.joinable()) thread_.join();
    }

    // True once `before` has run — i.e. the thread's queue/window exists and
    // FetchChunk is about to be entered.
    bool WaitUntilPrepared() {
        return PollUntil([this]() { return ready_.load(std::memory_order_acquire); },
                         kPollBudget);
    }

    DWORD thread_id() const { return tid_.load(std::memory_order_acquire); }

    // True when FetchChunk has returned within `budget`; caches the outcome.
    bool WaitFor(Ms budget) {
        if (have_outcome_) return true;
        if (future_.wait_for(budget) != std::future_status::ready) return false;
        outcome_      = future_.get();
        have_outcome_ = true;
        return true;
    }

    bool WaitForCompletion() { return WaitFor(kPollBudget); }

    const FetchOutcome& outcome() const { return outcome_; }

private:
    DispatcherChunkProvider&  provider_;
    std::function<void()>     before_;
    std::function<void()>     after_;
    std::future<FetchOutcome> future_;
    std::thread               thread_;
    std::atomic<DWORD>        tid_{0};
    std::atomic<bool>         ready_{false};
    FetchOutcome              outcome_;
    bool                      have_outcome_{false};
};

// Decode one captured frame into its FILE_CHUNK_REQUEST fields. `frame` must
// outlive the call (ParseHelperMessage hands out pointers into it).
::testing::AssertionResult DecodeChunkRequest(const Bytes&                   frame,
                                              codec::ParsedFileChunkRequest& out) {
    codec::ParsedHelperMessage outer;
    if (!codec::ParseHelperMessage(frame, outer)) {
        return ::testing::AssertionFailure() << "ParseHelperMessage rejected the frame";
    }
    if (static_cast<int>(outer.type) !=
        static_cast<int>(proto::HelperMessageType::FileChunkRequest)) {
        return ::testing::AssertionFailure()
               << "frame type is " << static_cast<int>(outer.type) << ", expected "
               << static_cast<int>(proto::HelperMessageType::FileChunkRequest);
    }
    if (outer.file_chunk_request_ptr == nullptr) {
        return ::testing::AssertionFailure() << "no file_chunk_request sub-message";
    }
    if (!codec::ParseFileChunkRequest(outer.file_chunk_request_ptr,
                                      outer.file_chunk_request_len, out)) {
        return ::testing::AssertionFailure() << "ParseFileChunkRequest failed";
    }
    return ::testing::AssertionSuccess();
}

// ─── Message-only window probe (rules 13 and 17) ──────────────────────────
//
// kPumpProbeMsg just records that it ran (rule 13). kFreezeProbeMsg parks the
// waiter thread inside the WndProc — i.e. inside FetchChunk's own message
// pump — until the test releases it, which is what lets rule 17 observe
// PendingCount() at a moment when the waiter provably cannot have woken.

constexpr UINT       kPumpProbeMsg   = WM_APP + 1;
constexpr UINT       kFreezeProbeMsg = WM_APP + 2;
const wchar_t* const kPumpProbeClass = L"LhChunkProviderPumpProbe";
std::atomic<int>     g_pump_hits{0};
std::atomic<HANDLE>  g_freeze_event{nullptr};
std::atomic<bool>    g_freeze_entered{false};

LRESULT CALLBACK PumpProbeWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kPumpProbeMsg) {
        g_pump_hits.fetch_add(1, std::memory_order_release);
        return 0;
    }
    if (msg == kFreezeProbeMsg) {
        g_freeze_entered.store(true, std::memory_order_release);
        HANDLE ev = g_freeze_event.load(std::memory_order_acquire);
        // Bounded, so a test that fails before releasing us cannot wedge the
        // waiter thread and turn a red test into a hang.
        if (ev != nullptr) {
            ::WaitForSingleObject(ev, static_cast<DWORD>(kPollBudget.count()));
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Creates the message-only window ON the waiter thread before FetchChunk and
// tears it down after: a window belongs to the thread that created it, and
// only that thread may pump for it. Declare one BEFORE the WaiterThread it
// serves so it outlives the join.
struct MessageOnlyProbe {
    std::atomic<HWND> hwnd{nullptr};

    std::function<void()> CreateHook() {
        return [this]() {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = &PumpProbeWndProc;
            wc.hInstance     = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = kPumpProbeClass;
            ::RegisterClassExW(&wc);
            hwnd.store(::CreateWindowExW(0, kPumpProbeClass, L"", 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr, wc.hInstance, nullptr),
                       std::memory_order_release);
        };
    }

    std::function<void()> DestroyHook() {
        return [this]() {
            HWND window = hwnd.exchange(nullptr, std::memory_order_acq_rel);
            if (window != nullptr) ::DestroyWindow(window);
            ::UnregisterClassW(kPumpProbeClass, ::GetModuleHandleW(nullptr));
        };
    }
};

// Closes a HANDLE on scope exit so an early ASSERT_* cannot leak one.
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr) ::CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle&)            = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

}  // namespace

// ─── Rule 1: construction ─────────────────────────────────────────────────

TEST(ChunkProviderConstruction, DefaultTimeoutIsThirtySeconds) {
    DispatcherChunkProvider provider(DispatcherChunkProvider::SendFrameFn{});
    EXPECT_EQ(provider.timeout_ms(), kDefaultChunkTimeoutMs);
    EXPECT_EQ(kDefaultChunkTimeoutMs, static_cast<DWORD>(30 * 1000));
}

TEST(ChunkProviderConstruction, ExplicitTimeoutIsEchoed) {
    Harness harness(kShortTimeoutMs);
    EXPECT_EQ(harness.provider().timeout_ms(), kShortTimeoutMs);

    Harness other(1234);
    EXPECT_EQ(other.provider().timeout_ms(), static_cast<DWORD>(1234));
}

TEST(ChunkProviderConstruction, FreshProviderHasNoPending) {
    Harness harness(kLongTimeoutMs);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(0));
}

// ─── Rule 2: no transport ─────────────────────────────────────────────────

TEST(ChunkProviderTransport, EmptySendFrameFailsImmediately) {
    // No SendFrameFn at all: there is nothing to send through, so the call
    // must fail before staging anything rather than block for the timeout.
    DispatcherChunkProvider provider(DispatcherChunkProvider::SendFrameFn{}, kLongTimeoutMs);

    Bytes data{0x11, 0x22};
    bool  is_last = true;
    const auto    start = std::chrono::steady_clock::now();
    const HRESULT hr    = provider.FetchChunk("tx", "f0", 0, 4096, data, is_last);
    const auto    elapsed =
        std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_LT(elapsed, kPromptMs);
    EXPECT_TRUE(data.empty());
    EXPECT_FALSE(is_last);
    EXPECT_EQ(provider.PendingCount(), static_cast<std::size_t>(0));
}

// ─── Rule 3: argument validation ──────────────────────────────────────────

TEST(ChunkProviderArgs, EmptyTransferIdIsInvalidArg) {
    Harness harness(kLongTimeoutMs);

    Bytes         data{0x99};
    bool          is_last = true;
    const HRESULT hr      = harness.provider().FetchChunk("", "f0", 0, 4096, data, is_last);

    EXPECT_EQ(hr, E_INVALIDARG);
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(0));
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
    EXPECT_TRUE(data.empty());
    EXPECT_FALSE(is_last);
}

TEST(ChunkProviderArgs, EmptyFileIdIsInvalidArg) {
    Harness harness(kLongTimeoutMs);

    Bytes data;
    bool  is_last = false;
    EXPECT_EQ(harness.provider().FetchChunk("tx", "", 0, 4096, data, is_last), E_INVALIDARG);
    // Both empty is still E_INVALIDARG as long as a transport exists.
    EXPECT_EQ(harness.provider().FetchChunk("", "", 0, 4096, data, is_last), E_INVALIDARG);

    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(0));
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderArgs, ZeroOffsetAndZeroSizeAreAccepted) {
    // offset == 0 / size == 0 are legal — the call must get as far as the
    // transport (which fails here, so the test does not have to block) and
    // must NOT come back E_INVALIDARG.
    Harness harness(kLongTimeoutMs, /*send_ok=*/false);

    Bytes         data;
    bool          is_last = false;
    const HRESULT hr      = harness.provider().FetchChunk("tx", "f0", 0, 0, data, is_last);

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_NE(hr, E_INVALIDARG);
    ASSERT_EQ(harness.frame_count(), static_cast<std::size_t>(1));

    const Bytes                   frame = harness.frame(0);
    codec::ParsedFileChunkRequest req;
    ASSERT_TRUE(DecodeChunkRequest(frame, req));
    EXPECT_EQ(req.offset, static_cast<std::uint64_t>(0));
    EXPECT_EQ(req.size, static_cast<std::uint32_t>(0));
}

// ─── Rule 4: send failure ─────────────────────────────────────────────────

TEST(ChunkProviderTransport, SendFailureFailsFastAndLeavesNoPending) {
    Harness harness(kLongTimeoutMs, /*send_ok=*/false);

    Bytes         data;
    bool          is_last = true;
    const auto    start = std::chrono::steady_clock::now();
    const HRESULT hr    = harness.provider().FetchChunk("tx", "f0", 128, 4096, data, is_last);
    const auto    elapsed =
        std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_LT(elapsed, kPromptMs) << "send failure waited for the timeout";
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "the staged slot survived a failed send";
    EXPECT_TRUE(data.empty());
    EXPECT_FALSE(is_last);
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(1));
}

// ─── Rule 5: the request frame ────────────────────────────────────────────

TEST(ChunkProviderRequestFrame, EncodesTupleAndIsSentBeforeBlocking) {
    Harness harness(kLongTimeoutMs);

    const std::string   transfer_id = "xfer-42";
    const std::string   file_id     = "file-7";
    const std::uint64_t offset      = 0x1122334455667788ull;
    const std::uint32_t size        = 65536u;

    WaiterThread waiter(harness.provider(), transfer_id, file_id, offset, size);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    ASSERT_EQ(harness.frame_count(), static_cast<std::size_t>(1))
        << "exactly one frame per FetchChunk";
    // The slot is staged before the frame goes out, which is what makes the
    // reply race-free: the pipe thread can already find us mid-send.
    EXPECT_EQ(harness.pending_during_last_send(), static_cast<std::size_t>(1))
        << "the frame was sent before the request was staged";

    const Bytes                   frame = harness.frame(0);
    codec::ParsedFileChunkRequest req;
    ASSERT_TRUE(DecodeChunkRequest(frame, req));
    EXPECT_EQ(req.transfer_id, transfer_id);
    EXPECT_EQ(req.file_id, file_id);
    EXPECT_EQ(req.offset, offset);
    EXPECT_EQ(req.size, size);

    harness.provider().DeliverChunk(transfer_id, file_id, offset, Bytes{0x01}, false, "");
    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(1))
        << "a completed request re-sent its frame";
}

// ─── Rule 6: happy path ───────────────────────────────────────────────────

TEST(ChunkProviderDelivery, DeliversBytesAndIsLastFalse) {
    Harness     harness(kLongTimeoutMs);
    const Bytes payload{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};

    WaiterThread waiter(harness.provider(), "tx", "f0", 4096, 8192);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    harness.provider().DeliverChunk("tx", "f0", 4096, payload, /*is_last=*/false, "");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(waiter.outcome().data, payload);
    EXPECT_FALSE(waiter.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderDelivery, DeliversIsLastTrue) {
    Harness     harness(kLongTimeoutMs);
    const Bytes payload{0x7F, 0x80, 0x81};

    WaiterThread waiter(harness.provider(), "tx", "f0", 0, 3);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    harness.provider().DeliverChunk("tx", "f0", 0, payload, /*is_last=*/true, "");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(waiter.outcome().data, payload);
    EXPECT_TRUE(waiter.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderDelivery, EmptyPayloadWithoutErrorIsSuccess) {
    // Zero bytes with no error is a legitimate reply — EOF is the stream's
    // business, not the provider's.
    Harness harness(kLongTimeoutMs);

    WaiterThread waiter(harness.provider(), "tx", "f0", 999, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    harness.provider().DeliverChunk("tx", "f0", 999, Bytes{}, /*is_last=*/true, "");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_TRUE(waiter.outcome().data.empty());
    EXPECT_TRUE(waiter.outcome().is_last);
}

// ─── Rule 7: correlation ──────────────────────────────────────────────────

TEST(ChunkProviderCorrelation, MismatchedKeysDoNotWakeWaiter) {
    Harness     harness(kLongTimeoutMs);
    const Bytes wrong{0xBA, 0xD0};
    const Bytes right{0x10, 0x20, 0x30};

    WaiterThread waiter(harness.provider(), "tx", "f0", 4096, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    harness.provider().DeliverChunk("other-tx", "f0", 4096, wrong, true, "");
    harness.provider().DeliverChunk("tx", "other-file", 4096, wrong, true, "");
    harness.provider().DeliverChunk("tx", "f0", 8192, wrong, true, "");

    // Give any mistaken wake-up time to happen, then confirm nothing moved.
    EXPECT_FALSE(waiter.WaitFor(kGrace)) << "a mismatched delivery woke the waiter";
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(1));

    harness.provider().DeliverChunk("tx", "f0", 4096, right, /*is_last=*/false, "");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(waiter.outcome().data, right) << "the waiter took a mismatched payload";
    EXPECT_FALSE(waiter.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderCorrelation, DeliveryWithNoWaiterIsSilentNoOp) {
    Harness harness(kLongTimeoutMs);

    harness.provider().DeliverChunk("nobody", "home", 0, Bytes{0x01, 0x02}, true, "");
    harness.provider().DeliverChunk("nobody", "home", 0, Bytes{}, false, "late");

    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "an unmatched delivery created state";
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(0));
}

// ─── Rule 8: parent error ─────────────────────────────────────────────────

TEST(ChunkProviderDelivery, ParentErrorFailsPromptly) {
    Harness harness(kLongTimeoutMs);

    WaiterThread waiter(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    // Bytes and is_last=true alongside the error: neither may reach the
    // caller, because an error reply is not a successful read.
    harness.provider().DeliverChunk("tx", "f0", 0, Bytes{0xAA, 0xBB},
                                    /*is_last=*/true, "upstream exploded");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, E_FAIL);
    EXPECT_TRUE(waiter.outcome().data.empty()) << "error reply leaked its payload";
    EXPECT_FALSE(waiter.outcome().is_last) << "error reply leaked is_last";
    EXPECT_LT(waiter.outcome().elapsed_ms, kPromptMs) << "error waited for the timeout";
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

// ─── Rule 9: timeout ──────────────────────────────────────────────────────

TEST(ChunkProviderTimeout, ExpiresNearTimeoutAndLateDeliveryIsNoOp) {
    Harness harness(kShortTimeoutMs);

    WaiterThread waiter(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(waiter.WaitFor(Ms(kShortTimeoutMs) + kPollBudget))
        << "FetchChunk never returned after its timeout";

    EXPECT_EQ(waiter.outcome().hr, E_FAIL);
    // GetTickCount has ~16 ms resolution, so allow a generous lower band;
    // the point is that it did NOT return early.
    EXPECT_GE(waiter.outcome().elapsed_ms, static_cast<long long>(kShortTimeoutMs) / 2)
        << "returned far too early to be the timeout firing";
    EXPECT_LT(waiter.outcome().elapsed_ms, static_cast<long long>(kShortTimeoutMs) + 4000);
    EXPECT_TRUE(waiter.outcome().data.empty());
    EXPECT_FALSE(waiter.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));

    // The reply the parent finally sends must land on nothing.
    harness.provider().DeliverChunk("tx", "f0", 0, Bytes{0x01, 0x02, 0x03}, true, "");
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "a post-timeout delivery left state behind";
}

// ─── Rule 10: cancellation ────────────────────────────────────────────────

TEST(ChunkProviderCancel, CancelAllRevertsInFlightWaiter) {
    Harness harness(kLongTimeoutMs);

    WaiterThread waiter(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    harness.provider().CancelAll();

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, STG_E_REVERTED);
    EXPECT_TRUE(waiter.outcome().data.empty());
    EXPECT_FALSE(waiter.outcome().is_last);
    EXPECT_LT(waiter.outcome().elapsed_ms, kPromptMs) << "cancel waited for the timeout";
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderCancel, CancelAllWithNothingInFlightIsNoOp) {
    Harness harness(kLongTimeoutMs);

    harness.provider().CancelAll();
    harness.provider().CancelAll();

    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderCancel, CancellationIsNotSticky) {
    // CancelAll flags the slots that exist; it installs no process-wide gate,
    // so the next announcement's FetchChunk is served normally.
    Harness harness(kLongTimeoutMs);

    {
        WaiterThread first(harness.provider(), "tx", "f0", 0, 4096);
        ASSERT_TRUE(WaitForPending(harness.provider(), 1));
        harness.provider().CancelAll();
        ASSERT_TRUE(first.WaitForCompletion());
        ASSERT_EQ(first.outcome().hr, STG_E_REVERTED);
    }
    ASSERT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));

    const Bytes  payload{0x5A, 0x5B};
    WaiterThread second(harness.provider(), "tx2", "f1", 16, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    harness.provider().DeliverChunk("tx2", "f1", 16, payload, /*is_last=*/true, "");

    ASSERT_TRUE(second.WaitForCompletion());
    EXPECT_EQ(second.outcome().hr, S_OK) << "cancellation was sticky";
    EXPECT_EQ(second.outcome().data, payload);
    EXPECT_TRUE(second.outcome().is_last);
}

// ─── Rule 11: several waiters on distinct keys ────────────────────────────

TEST(ChunkProviderMultiWaiter, DifferentKeysAreIndependent) {
    Harness     harness(kLongTimeoutMs);
    const Bytes payload_a{0xA0, 0xA1};
    const Bytes payload_b{0xB0, 0xB1, 0xB2};

    WaiterThread a(harness.provider(), "tx", "fileA", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    WaiterThread b(harness.provider(), "tx", "fileB", 4096, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 2));
    ASSERT_EQ(harness.frame_count(), static_cast<std::size_t>(2));

    harness.provider().DeliverChunk("tx", "fileA", 0, payload_a, /*is_last=*/false, "");

    ASSERT_TRUE(a.WaitForCompletion());
    EXPECT_EQ(a.outcome().hr, S_OK);
    EXPECT_EQ(a.outcome().data, payload_a);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    EXPECT_FALSE(b.WaitFor(kGrace)) << "delivering A also woke B";

    harness.provider().DeliverChunk("tx", "fileB", 4096, payload_b, /*is_last=*/true, "");

    ASSERT_TRUE(b.WaitForCompletion());
    EXPECT_EQ(b.outcome().hr, S_OK);
    EXPECT_EQ(b.outcome().data, payload_b);
    EXPECT_TRUE(b.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderMultiWaiter, CancelAllWakesEveryWaiter) {
    Harness harness(kLongTimeoutMs);

    WaiterThread a(harness.provider(), "tx", "fileA", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    WaiterThread b(harness.provider(), "tx", "fileB", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 2));

    harness.provider().CancelAll();

    ASSERT_TRUE(a.WaitForCompletion());
    ASSERT_TRUE(b.WaitForCompletion());
    EXPECT_EQ(a.outcome().hr, STG_E_REVERTED);
    EXPECT_EQ(b.outcome().hr, STG_E_REVERTED);
    EXPECT_TRUE(a.outcome().data.empty());
    EXPECT_TRUE(b.outcome().data.empty());
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

// ─── Rule 12: same key, FIFO ──────────────────────────────────────────────

TEST(ChunkProviderMultiWaiter, SameKeyIsServedFifo) {
    Harness     harness(kLongTimeoutMs);
    const Bytes first_payload{0x01, 0x11};
    const Bytes second_payload{0x02, 0x22, 0x33};

    WaiterThread older(harness.provider(), "tx", "f0", 0, 4096);
    // Only start the second once the first is staged, so "older" is defined.
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    WaiterThread newer(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 2));
    ASSERT_EQ(harness.frame_count(), static_cast<std::size_t>(2))
        << "each FetchChunk sends its own request";

    harness.provider().DeliverChunk("tx", "f0", 0, first_payload, /*is_last=*/false, "");

    ASSERT_TRUE(older.WaitForCompletion()) << "the older waiter was not the one completed";
    EXPECT_EQ(older.outcome().hr, S_OK);
    EXPECT_EQ(older.outcome().data, first_payload);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    EXPECT_FALSE(newer.WaitFor(kGrace)) << "one delivery completed both waiters";

    harness.provider().DeliverChunk("tx", "f0", 0, second_payload, /*is_last=*/true, "");

    ASSERT_TRUE(newer.WaitForCompletion()) << "the second waiter was stranded";
    EXPECT_EQ(newer.outcome().hr, S_OK);
    EXPECT_EQ(newer.outcome().data, second_payload);
    EXPECT_TRUE(newer.outcome().is_last);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

// ─── Rule 16: back-to-back replies for one tuple ──────────────────────────
//
// The pipe thread has no interlock with the STA: two FILE_CHUNK_DATA replies
// for the same tuple can arrive one after the other before either waiter has
// been scheduled. A reply must therefore CONSUME its request at delivery
// time — otherwise the second reply finds the first waiter's slot again,
// overwrites bytes it has not read yet, and strands the second waiter until
// its timeout.

TEST(ChunkProviderMultiWaiter, BackToBackRepliesCompleteBothWaitersInOrder) {
    Harness     harness(kLongTimeoutMs);
    const Bytes first_payload{0x11, 0x22, 0x33};
    const Bytes second_payload{0x44, 0x55};

    WaiterThread older(harness.provider(), "tx", "f0", 512, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    WaiterThread newer(harness.provider(), "tx", "f0", 512, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 2));

    // No waiting for `older` in between — this is the whole point.
    harness.provider().DeliverChunk("tx", "f0", 512, first_payload, /*is_last=*/false, "");
    harness.provider().DeliverChunk("tx", "f0", 512, second_payload, /*is_last=*/true, "");

    // Deterministic: each delivery popped its request while holding the lock,
    // so the count is already 0 here whether or not either waiter has woken.
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "a reply did not consume its request at delivery time";

    ASSERT_TRUE(older.WaitForCompletion()) << "the first waiter never completed";
    EXPECT_EQ(older.outcome().hr, S_OK);
    EXPECT_EQ(older.outcome().data, first_payload)
        << "the second reply overwrote the first waiter's bytes";
    EXPECT_FALSE(older.outcome().is_last);

    ASSERT_TRUE(newer.WaitForCompletion()) << "the second waiter was stranded";
    EXPECT_EQ(newer.outcome().hr, S_OK);
    EXPECT_EQ(newer.outcome().data, second_payload);
    EXPECT_TRUE(newer.outcome().is_last);
}

TEST(ChunkProviderMultiWaiter, BackToBackErrorThenSuccessCompletesBothWaiters) {
    // Same shape, but the first reply is a parent error: the error must reach
    // exactly the first waiter and must not contaminate the second.
    Harness     harness(kLongTimeoutMs);
    const Bytes payload{0x9A, 0x9B, 0x9C};

    WaiterThread older(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    WaiterThread newer(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 2));

    harness.provider().DeliverChunk("tx", "f0", 0, Bytes{0xEE}, /*is_last=*/true, "boom");
    harness.provider().DeliverChunk("tx", "f0", 0, payload, /*is_last=*/true, "");

    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "a reply did not consume its request at delivery time";

    ASSERT_TRUE(older.WaitForCompletion());
    EXPECT_EQ(older.outcome().hr, E_FAIL);
    EXPECT_TRUE(older.outcome().data.empty());
    EXPECT_FALSE(older.outcome().is_last);

    ASSERT_TRUE(newer.WaitForCompletion()) << "the second waiter was stranded";
    EXPECT_EQ(newer.outcome().hr, S_OK) << "the first reply's error leaked into the second";
    EXPECT_EQ(newer.outcome().data, payload);
    EXPECT_TRUE(newer.outcome().is_last);
}

// ─── Rule 13: the wait pumps window messages ──────────────────────────────

TEST(ChunkProviderPump, DispatchesWindowMessagesWhileWaiting) {
    Harness harness(kLongTimeoutMs);
    g_pump_hits.store(0, std::memory_order_release);

    MessageOnlyProbe probe;
    WaiterThread     waiter(harness.provider(), "tx", "f0", 0, 4096, probe.CreateHook(),
                            probe.DestroyHook());
    ASSERT_TRUE(waiter.WaitUntilPrepared());
    HWND hwnd = probe.hwnd.load(std::memory_order_acquire);
    ASSERT_NE(hwnd, nullptr) << "message-only window was not created";
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    ASSERT_NE(::PostMessageW(hwnd, kPumpProbeMsg, 0, 0), FALSE);

    const bool dispatched =
        PollUntil([]() { return g_pump_hits.load(std::memory_order_acquire) > 0; },
                  kPollBudget);
    EXPECT_TRUE(dispatched) << "FetchChunk did not pump the thread's message queue";
    // Still blocked: the pump must not have ended the wait.
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(1));
    EXPECT_FALSE(waiter.WaitFor(Ms(0)));

    harness.provider().DeliverChunk("tx", "f0", 0, Bytes{0x77}, /*is_last=*/true, "");

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(waiter.outcome().data, Bytes{0x77});
    EXPECT_EQ(g_pump_hits.load(std::memory_order_acquire), 1);
}

// ─── Rule 14: WM_QUIT ─────────────────────────────────────────────────────

TEST(ChunkProviderPump, QuitMessageAbortsAndIsReposted) {
    Harness harness(kLongTimeoutMs);

    std::atomic<bool> quit_still_queued{false};

    // PeekMessage with PM_NOREMOVE forces the thread's message queue into
    // existence, so PostThreadMessageW from the test thread can land.
    auto make_queue = []() {
        MSG msg{};
        ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    };
    // After FetchChunk returns, the re-posted quit must still be sitting in
    // this thread's queue for the STA's outer loop to find.
    auto drain_quit = [&quit_still_queued]() {
        MSG        msg{};
        const BOOL got = ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        quit_still_queued.store(got != FALSE && msg.message == WM_QUIT,
                                std::memory_order_release);
    };

    WaiterThread waiter(harness.provider(), "tx", "f0", 0, 4096, make_queue, drain_quit);
    ASSERT_TRUE(waiter.WaitUntilPrepared());
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    const DWORD tid = waiter.thread_id();
    ASSERT_NE(tid, static_cast<DWORD>(0));
    // The queue may still be materialising; retry briefly rather than
    // failing on a transient ERROR_INVALID_THREAD_ID.
    const bool posted = PollUntil(
        [tid]() { return ::PostThreadMessageW(tid, WM_QUIT, 0, 0) != FALSE; }, kPollBudget);
    ASSERT_TRUE(posted) << "PostThreadMessageW never reached the waiter thread";

    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, E_ABORT);
    EXPECT_TRUE(waiter.outcome().data.empty());
    EXPECT_FALSE(waiter.outcome().is_last);
    EXPECT_LT(waiter.outcome().elapsed_ms, kPromptMs) << "WM_QUIT waited for the timeout";
    EXPECT_TRUE(quit_still_queued.load(std::memory_order_acquire))
        << "WM_QUIT was swallowed instead of re-posted";
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

// ─── Rule 17: the reply, not the wake-up, clears the slot ─────────────────

TEST(ChunkProviderPump, PendingDropsWhenTheReplyLandsNotWhenTheWaiterWakes) {
    // Park the waiter thread inside our WndProc — which runs inside
    // FetchChunk's own message pump — so it demonstrably cannot have returned
    // from FetchChunk. Anything that changes PendingCount() while it sits
    // there can only be the delivery itself. No sleeps: the freeze is an
    // event the test owns, and every wait around it is bounded.
    Harness     harness(kLongTimeoutMs);
    const Bytes payload{0xC0, 0xDE};

    g_freeze_entered.store(false, std::memory_order_release);
    ScopedHandle freeze(::CreateEventW(nullptr, /*manualReset=*/TRUE,
                                       /*initial=*/FALSE, nullptr));
    ASSERT_NE(freeze.get(), nullptr);
    g_freeze_event.store(freeze.get(), std::memory_order_release);

    // `probe` and `freeze` are declared before the waiter, so they outlive the
    // join its destructor performs.
    MessageOnlyProbe probe;
    WaiterThread     waiter(harness.provider(), "tx", "f0", 0, 4096, probe.CreateHook(),
                            probe.DestroyHook());
    ASSERT_TRUE(waiter.WaitUntilPrepared());
    HWND hwnd = probe.hwnd.load(std::memory_order_acquire);
    ASSERT_NE(hwnd, nullptr) << "message-only window was not created";
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));

    ASSERT_NE(::PostMessageW(hwnd, kFreezeProbeMsg, 0, 0), FALSE);
    ASSERT_TRUE(PollUntil([]() { return g_freeze_entered.load(std::memory_order_acquire); },
                          kPollBudget))
        << "the waiter never dispatched the freeze message";

    harness.provider().DeliverChunk("tx", "f0", 0, payload, /*is_last=*/true, "");

    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0))
        << "the slot was still counted after its reply landed";
    EXPECT_FALSE(waiter.WaitFor(Ms(0)))
        << "the waiter was not actually frozen — the observation above proves nothing";

    ASSERT_NE(::SetEvent(freeze.get()), FALSE);
    ASSERT_TRUE(waiter.WaitForCompletion());
    EXPECT_EQ(waiter.outcome().hr, S_OK);
    EXPECT_EQ(waiter.outcome().data, payload) << "the staged bytes did not survive the pump";
    EXPECT_TRUE(waiter.outcome().is_last);
    g_freeze_event.store(nullptr, std::memory_order_release);
}

// ─── Rule 15: reuse after every outcome ───────────────────────────────────

TEST(ChunkProviderReuse, SameTupleAfterSuccessErrorAndCancel) {
    Harness             harness(kLongTimeoutMs);
    const std::string   transfer_id = "tx";
    const std::string   file_id     = "f0";
    const std::uint64_t offset      = 2048;

    // 1) success
    {
        WaiterThread w(harness.provider(), transfer_id, file_id, offset, 4096);
        ASSERT_TRUE(WaitForPending(harness.provider(), 1));
        harness.provider().DeliverChunk(transfer_id, file_id, offset, Bytes{0x01}, false, "");
        ASSERT_TRUE(w.WaitForCompletion());
        ASSERT_EQ(w.outcome().hr, S_OK);
    }
    // 2) parent error
    {
        WaiterThread w(harness.provider(), transfer_id, file_id, offset, 4096);
        ASSERT_TRUE(WaitForPending(harness.provider(), 1));
        harness.provider().DeliverChunk(transfer_id, file_id, offset, Bytes{}, false, "nope");
        ASSERT_TRUE(w.WaitForCompletion());
        ASSERT_EQ(w.outcome().hr, E_FAIL);
    }
    // 3) cancellation
    {
        WaiterThread w(harness.provider(), transfer_id, file_id, offset, 4096);
        ASSERT_TRUE(WaitForPending(harness.provider(), 1));
        harness.provider().CancelAll();
        ASSERT_TRUE(w.WaitForCompletion());
        ASSERT_EQ(w.outcome().hr, STG_E_REVERTED);
    }
    // 4) the same tuple is still served
    const Bytes  payload{0xFE, 0xED};
    WaiterThread last(harness.provider(), transfer_id, file_id, offset, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    harness.provider().DeliverChunk(transfer_id, file_id, offset, payload, true, "");

    ASSERT_TRUE(last.WaitForCompletion());
    EXPECT_EQ(last.outcome().hr, S_OK);
    EXPECT_EQ(last.outcome().data, payload);
    EXPECT_TRUE(last.outcome().is_last);
    EXPECT_EQ(harness.frame_count(), static_cast<std::size_t>(4));
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}

TEST(ChunkProviderReuse, SameTupleAfterTimeout) {
    Harness harness(kShortTimeoutMs);

    {
        WaiterThread stale(harness.provider(), "tx", "f0", 0, 4096);
        ASSERT_TRUE(stale.WaitFor(Ms(kShortTimeoutMs) + kPollBudget));
        ASSERT_EQ(stale.outcome().hr, E_FAIL);
    }
    ASSERT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));

    const Bytes  payload{0x0F, 0xF0};
    WaiterThread fresh(harness.provider(), "tx", "f0", 0, 4096);
    ASSERT_TRUE(WaitForPending(harness.provider(), 1));
    harness.provider().DeliverChunk("tx", "f0", 0, payload, /*is_last=*/false, "");

    ASSERT_TRUE(fresh.WaitForCompletion());
    EXPECT_EQ(fresh.outcome().hr, S_OK) << "the tuple was poisoned by an earlier timeout";
    EXPECT_EQ(fresh.outcome().data, payload);
    EXPECT_EQ(harness.provider().PendingCount(), static_cast<std::size_t>(0));
}
