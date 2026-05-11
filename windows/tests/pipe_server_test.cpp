// Integration tests for PipeServer: spin up a real named-pipe server on a
// worker thread, connect a Win32 client via CreateFileW, and exercise the
// length-prefix framing protocol end-to-end.

#include "pipe_server.h"
#include "test_lite.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using leviathan::clipboard_helper::PipeServer;

namespace {

constexpr DWORD kClientIoTimeoutMs = 5000;
constexpr DWORD kRunJoinTimeoutMs  = 5000;

std::wstring MakeUniqueTestPipeName() {
    // The leading \\.\pipe\ prefix and a process+counter suffix keep test
    // runs isolated even when several tests are exercised back-to-back
    // and FILE_FLAG_FIRST_PIPE_INSTANCE has not yet released the slot.
    static std::atomic<unsigned long> counter{0};
    wchar_t buf[160];
    std::swprintf(buf, 160,
                  L"\\\\.\\pipe\\lh-helper-test-%lu-%lu",
                  ::GetCurrentProcessId(),
                  counter.fetch_add(1));
    return buf;
}

// RAII handle wrapper.
class Handle {
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    void reset() {
        if (valid()) { ::CloseHandle(h_); }
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_;
};

// Open the client side of a named pipe with overlapped I/O so the test can
// time-bound reads and writes instead of deadlocking if the server never
// responds.
Handle ConnectClient(const std::wstring& pipe_name, DWORD wait_ms = 2000) {
    // WaitNamedPipeW with a generous timeout — the PipeServer worker may
    // not have called CreateNamedPipeW yet when this test reaches here.
    if (!::WaitNamedPipeW(pipe_name.c_str(), wait_ms)) {
        // WaitNamedPipeW returns FALSE+ERROR_FILE_NOT_FOUND when the named
        // server hasn't been published yet; retry a few times to absorb the
        // tiny CreateNamedPipeW race after Run() starts.
        for (int i = 0; i < 50; ++i) {
            ::Sleep(20);
            if (::WaitNamedPipeW(pipe_name.c_str(), 100)) break;
        }
    }
    HANDLE h = ::CreateFileW(pipe_name.c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED,
                             nullptr);
    return Handle{h};
}

bool WaitOverlapped(HANDLE pipe, OVERLAPPED& ov, DWORD timeout_ms, DWORD& out_transferred) {
    const DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &ov);
        DWORD t = 0;
        ::GetOverlappedResult(pipe, &ov, &t, /*bWait=*/TRUE);
        return false;
    }
    return ::GetOverlappedResult(pipe, &ov, &out_transferred, /*bWait=*/FALSE) ? true : false;
}

bool WriteAll(HANDLE pipe, const void* data, DWORD count, DWORD timeout_ms) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    DWORD remaining = count;
    while (remaining > 0) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) return false;
        DWORD wrote = 0;
        BOOL ok = ::WriteFile(pipe, p, remaining, &wrote, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                ::CloseHandle(ov.hEvent);
                return false;
            }
            if (!WaitOverlapped(pipe, ov, timeout_ms, wrote)) {
                ::CloseHandle(ov.hEvent);
                return false;
            }
        }
        ::CloseHandle(ov.hEvent);
        if (wrote == 0) return false;
        p        += wrote;
        remaining -= wrote;
    }
    return true;
}

bool ReadAll(HANDLE pipe, void* data, DWORD count, DWORD timeout_ms) {
    std::uint8_t* p = static_cast<std::uint8_t*>(data);
    DWORD remaining = count;
    while (remaining > 0) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) return false;
        DWORD got = 0;
        BOOL ok = ::ReadFile(pipe, p, remaining, &got, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                ::CloseHandle(ov.hEvent);
                return false;
            }
            if (!WaitOverlapped(pipe, ov, timeout_ms, got)) {
                ::CloseHandle(ov.hEvent);
                return false;
            }
        }
        ::CloseHandle(ov.hEvent);
        if (got == 0) return false;
        p        += got;
        remaining -= got;
    }
    return true;
}

bool WriteFrame(HANDLE pipe, const std::vector<std::uint8_t>& payload, DWORD timeout_ms) {
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    if (!WriteAll(pipe, &len, sizeof(len), timeout_ms)) return false;
    if (len == 0) return true;
    return WriteAll(pipe, payload.data(), len, timeout_ms);
}

bool ReadFrame(HANDLE pipe, std::vector<std::uint8_t>& out_payload, DWORD timeout_ms) {
    std::uint32_t len = 0;
    if (!ReadAll(pipe, &len, sizeof(len), timeout_ms)) return false;
    if (len > 64 * 1024 * 1024) return false;
    out_payload.resize(len);
    if (len == 0) return true;
    return ReadAll(pipe, out_payload.data(), len, timeout_ms);
}

// Helper that wraps a PipeServer + the worker thread that runs it.
//
// The PipeServer is heap-allocated via shared_ptr because Stop() supports a
// bounded join timeout: if the worker thread somehow wedges, Stop() returns
// and the fixture destructor exits without joining. The detached worker
// still references the PipeServer, so the shared_ptr captured by the
// worker lambda keeps the object alive until the thread really exits.
class ServerFixture {
public:
    ServerFixture(const std::wstring& pipe_name,
                  leviathan::clipboard_helper::MessageHandler handler)
        : server_(std::make_shared<PipeServer>(pipe_name, std::move(handler))) {}

    PipeServer& server() { return *server_; }

    void Start() {
        // Capture server_ by value into the worker so the PipeServer
        // outlives any future timeout-Stop that abandons the thread.
        auto s = server_;
        worker_ = std::thread([s]() { s->Run(); });
    }

    void Stop() {
        if (!server_) return;
        server_->Stop();
        if (!worker_.joinable()) return;

        // std::thread::join has no timeout overload, so a wedged Run()
        // would hang the entire test process. Hand the worker off to a
        // detached helper that joins on our behalf, then poll a flag
        // until the join completes or the deadline expires.
        auto worker_holder = std::make_shared<std::thread>(std::move(worker_));
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread([worker_holder, done]() {
            worker_holder->join();
            done->store(true);
        }).detach();

        using namespace std::chrono;
        const auto deadline = steady_clock::now()
                            + milliseconds(kRunJoinTimeoutMs);
        while (!done->load() && steady_clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(20));
        }
        if (!done->load()) {
            std::fprintf(stderr,
                         "  ServerFixture::Stop: worker did not join within %lu ms — leaking detached worker (PipeServer kept alive by worker lambda)\n",
                         static_cast<unsigned long>(kRunJoinTimeoutMs));
        }
    }

    ~ServerFixture() { Stop(); }

private:
    std::shared_ptr<PipeServer> server_;
    std::thread                 worker_;
};

}  // namespace

// ─── Echo / round-trip ────────────────────────────────────────────────────

TEST(PipeServer, HandlerReceivesPayloadAndResponseIsDelivered) {
    const auto pipe_name = MakeUniqueTestPipeName();

    std::vector<std::uint8_t> seen_request;
    std::mutex                 seen_mu;
    std::condition_variable    seen_cv;

    leviathan::clipboard_helper::MessageHandler handler =
        [&](const std::vector<std::uint8_t>& req) {
            {
                std::lock_guard<std::mutex> lock(seen_mu);
                seen_request = req;
            }
            seen_cv.notify_all();
            // Echo with a one-byte prefix so the response is observably
            // distinct from the request (mirrors phase 1 protocol).
            std::vector<std::uint8_t> reply;
            reply.reserve(req.size() + 1);
            reply.push_back(0x01);
            reply.insert(reply.end(), req.begin(), req.end());
            return reply;
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    const std::vector<std::uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(WriteFrame(client.get(), payload, kClientIoTimeoutMs));

    std::vector<std::uint8_t> reply;
    ASSERT_TRUE(ReadFrame(client.get(), reply, kClientIoTimeoutMs));
    ASSERT_EQ(reply.size(), payload.size() + 1);
    EXPECT_EQ(static_cast<int>(reply[0]), 0x01);
    EXPECT_EQ(std::memcmp(reply.data() + 1, payload.data(), payload.size()), 0);

    {
        std::unique_lock<std::mutex> lock(seen_mu);
        ASSERT_TRUE(seen_cv.wait_for(lock, std::chrono::seconds(2),
                                     [&]() { return !seen_request.empty(); }));
        EXPECT_EQ(seen_request, payload);
    }
}

// ─── OnConnect frame ──────────────────────────────────────────────────────

TEST(PipeServer, OnConnectHandlerPushesInitialFrameBeforeAnyRequest) {
    const auto pipe_name = MakeUniqueTestPipeName();
    const std::vector<std::uint8_t> ready_payload = {'R', 'E', 'A', 'D', 'Y'};

    leviathan::clipboard_helper::MessageHandler handler =
        [](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            return {};
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.server().SetOnConnect([&]() { return ready_payload; });
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    // The server's first action after accepting must be to push the
    // on-connect frame — read it BEFORE writing any request.
    std::vector<std::uint8_t> initial;
    ASSERT_TRUE(ReadFrame(client.get(), initial, kClientIoTimeoutMs));
    EXPECT_EQ(initial, ready_payload);
}

// ─── Handler returns empty: no reply ──────────────────────────────────────

TEST(PipeServer, HandlerReturningEmptyDoesNotProduceAReply) {
    const auto pipe_name = MakeUniqueTestPipeName();
    std::atomic<int> calls{0};

    leviathan::clipboard_helper::MessageHandler handler =
        [&](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            calls.fetch_add(1);
            return {};  // explicit "no reply"
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    // Send two requests back-to-back. The handler gets both; the client
    // never reads a frame (because there is no reply).
    ASSERT_TRUE(WriteFrame(client.get(), {0x01}, kClientIoTimeoutMs));
    ASSERT_TRUE(WriteFrame(client.get(), {0x02}, kClientIoTimeoutMs));

    // Give the server a moment to process both.
    for (int i = 0; i < 100 && calls.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(calls.load(), 2);
}

// ─── Multiple frames per connection ───────────────────────────────────────

TEST(PipeServer, ConnectionHandlesMultipleSequentialFrames) {
    const auto pipe_name = MakeUniqueTestPipeName();

    leviathan::clipboard_helper::MessageHandler handler =
        [](const std::vector<std::uint8_t>& req) -> std::vector<std::uint8_t> {
            std::vector<std::uint8_t> reply = req;
            // Tag each byte so we can verify ordering.
            for (auto& b : reply) b = static_cast<std::uint8_t>(b + 1);
            return reply;
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    for (int i = 0; i < 5; ++i) {
        const std::vector<std::uint8_t> payload = {
            static_cast<std::uint8_t>(0x10 + i)
        };
        ASSERT_TRUE(WriteFrame(client.get(), payload, kClientIoTimeoutMs));

        std::vector<std::uint8_t> reply;
        ASSERT_TRUE(ReadFrame(client.get(), reply, kClientIoTimeoutMs));
        ASSERT_EQ(reply.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(static_cast<int>(reply[0]), 0x10 + i + 1);
    }
}

// ─── Oversized frame triggers disconnect ──────────────────────────────────

TEST(PipeServer, FrameLargerThanCapForcesDisconnect) {
    const auto pipe_name = MakeUniqueTestPipeName();

    leviathan::clipboard_helper::MessageHandler handler =
        [](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            return {};
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    // Write a length prefix claiming 17 MB — the server caps at 16 MB.
    const std::uint32_t bogus_len = 17u * 1024u * 1024u;
    ASSERT_TRUE(WriteAll(client.get(), &bogus_len, sizeof(bogus_len), kClientIoTimeoutMs));

    // After the server rejects the oversized frame, ReadFrame on our side
    // observes a broken pipe (the server closed it). The read may take a
    // moment to propagate — give it generous slack.
    std::vector<std::uint8_t> reply;
    EXPECT_FALSE(ReadFrame(client.get(), reply, kClientIoTimeoutMs));
}

// ─── SendFrame from another thread ────────────────────────────────────────

TEST(PipeServer, SendFrameDeliversUnsolicitedFrameToActiveClient) {
    const auto pipe_name = MakeUniqueTestPipeName();
    std::atomic<bool> received_request{false};

    leviathan::clipboard_helper::MessageHandler handler =
        [&](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            received_request.store(true);
            return {};
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    // Send a request so the server's ServeOneClient loop has the active
    // pipe published; without that, SendFrame has no client to push to.
    ASSERT_TRUE(WriteFrame(client.get(), {0xA0}, kClientIoTimeoutMs));
    for (int i = 0; i < 100 && !received_request.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(received_request.load());

    const std::vector<std::uint8_t> push_payload = {'p', 'u', 's', 'h'};
    EXPECT_TRUE(fixture.server().SendFrame(push_payload));

    std::vector<std::uint8_t> pushed;
    ASSERT_TRUE(ReadFrame(client.get(), pushed, kClientIoTimeoutMs));
    EXPECT_EQ(pushed, push_payload);
}

TEST(PipeServer, SendFrameFailsWhenNoClientIsConnected) {
    const auto pipe_name = MakeUniqueTestPipeName();

    leviathan::clipboard_helper::MessageHandler handler =
        [](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            return {};
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();
    // No client connects.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(fixture.server().SendFrame({'x'}));
}

// ─── Zero-length frame ────────────────────────────────────────────────────

TEST(PipeServer, ZeroLengthFrameIsValidAndHandlerSeesEmptyPayload) {
    const auto pipe_name = MakeUniqueTestPipeName();

    std::atomic<int> calls{0};
    std::atomic<std::size_t> last_size{1};  // sentinel non-zero

    leviathan::clipboard_helper::MessageHandler handler =
        [&](const std::vector<std::uint8_t>& req) -> std::vector<std::uint8_t> {
            calls.fetch_add(1);
            last_size.store(req.size());
            return {0x55};  // single-byte reply so the test can sync
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();

    Handle client = ConnectClient(pipe_name);
    ASSERT_TRUE(client.valid());

    ASSERT_TRUE(WriteFrame(client.get(), {}, kClientIoTimeoutMs));

    std::vector<std::uint8_t> reply;
    ASSERT_TRUE(ReadFrame(client.get(), reply, kClientIoTimeoutMs));
    EXPECT_EQ(reply.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(static_cast<int>(reply[0]), 0x55);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(last_size.load(), static_cast<std::size_t>(0));
}

// ─── Stop unblocks Run() ──────────────────────────────────────────────────

TEST(PipeServer, StopUnblocksRunEvenWhenNoClientHasConnected) {
    const auto pipe_name = MakeUniqueTestPipeName();
    leviathan::clipboard_helper::MessageHandler handler =
        [](const std::vector<std::uint8_t>&) -> std::vector<std::uint8_t> {
            return {};
        };

    ServerFixture fixture(pipe_name, handler);
    fixture.Start();
    // No client connects; Run() is parked inside ConnectClient's
    // WaitForMultipleObjects on the overlapped event + stop_event_.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto start = std::chrono::steady_clock::now();
    fixture.Stop();  // joins inside
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        static_cast<long long>(kRunJoinTimeoutMs));
}
