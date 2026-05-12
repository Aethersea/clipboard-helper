#include "pipe_server.h"

#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;
constexpr DWORD kPipeDefaultTimeoutMs = 5000;
constexpr std::uint32_t kMaxFrameSize = 16 * 1024 * 1024;  // 16 MiB hard cap

// Build a security descriptor that grants generic R/W to the current user and
// LocalSystem, denies network logon, and excludes everyone else.
//
// SDDL grammar:
//   D:dacl_flags(ace_type;ace_flags;rights;object_guid;inherit_object_guid;account_sid)
//
// We grant FILE_GENERIC_READ | FILE_GENERIC_WRITE (mapped to "GA") to:
//   - SY  : Local System
//   - OW  : the pipe creator (helper process user)
//
// Network logons are denied via "(D;;GA;;;NU)".
PSECURITY_DESCRIPTOR BuildPipeSecurityDescriptor() {
    constexpr const wchar_t* kSddl =
        L"D:(D;;GA;;;NU)"          // deny Network Users
        L"(A;;GRGW;;;SY)"          // allow SYSTEM read+write
        L"(A;;GRGWGX;;;OW)";       // allow owner (helper user) read+write+execute

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSddl, SDDL_REVISION_1, &sd, nullptr)) {
        LH_LOG_ERROR("ConvertStringSecurityDescriptorToSecurityDescriptorW failed");
        return nullptr;
    }
    return sd;
}

// RAII wrapper for the per-operation OVERLAPPED + event handle pair, so that
// every exit path closes the event without manual bookkeeping.
class OverlappedOp {
public:
    OverlappedOp() {
        std::memset(&ov_, 0, sizeof(ov_));
        ov_.hEvent = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    }
    ~OverlappedOp() {
        if (ov_.hEvent) ::CloseHandle(ov_.hEvent);
    }
    OverlappedOp(const OverlappedOp&) = delete;
    OverlappedOp& operator=(const OverlappedOp&) = delete;

    OVERLAPPED* get() { return &ov_; }
    HANDLE event() const { return ov_.hEvent; }
    bool valid() const  { return ov_.hEvent != nullptr; }

private:
    OVERLAPPED ov_;
};

}  // namespace

PipeServer::PipeServer(std::wstring pipe_name, MessageHandler handler)
    : pipe_name_(std::move(pipe_name)), handler_(std::move(handler)) {
    stop_event_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    if (stop_event_ == nullptr) {
        LH_LOG_ERROR("CreateEventW for stop_event_ failed");
    }
}

PipeServer::~PipeServer() {
    // Run() is responsible for joining writer_thread_, but if the caller
    // destroyed us before Run() returned (or Run() was never called yet
    // a thread happened to be spawned, e.g. via a future direct API), we
    // must avoid std::terminate from a joinable-thread destructor.
    if (writer_thread_.joinable()) {
        stop_.store(true);
        outbound_cv_.notify_all();
        if (stop_event_) ::SetEvent(stop_event_);
        writer_thread_.join();
    }
    if (stop_event_) {
        ::CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

void PipeServer::SetOnConnect(OnConnectHandler on_connect) {
    on_connect_ = std::move(on_connect);
}

void PipeServer::Stop() {
    stop_.store(true);
    if (stop_event_) {
        ::SetEvent(stop_event_);
    }
    // Nudge the writer thread so it observes stop_ and drains/exits
    // promptly instead of blocking forever on the condition variable.
    outbound_cv_.notify_all();
}

bool PipeServer::CreatePipe(HANDLE& out_handle) {
    PSECURITY_DESCRIPTOR sd = BuildPipeSecurityDescriptor();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;

    // FILE_FLAG_OVERLAPPED is required so ConnectNamedPipe / ReadFile / WriteFile
    // can be cancelled cooperatively via the shutdown event.
    HANDLE pipe = ::CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,                       // single instance
        kPipeBufferSize,         // outBufferSize
        kPipeBufferSize,         // inBufferSize
        kPipeDefaultTimeoutMs,
        sd ? &sa : nullptr);

    if (sd) {
        ::LocalFree(sd);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "CreateNamedPipeW failed: lastError=%lu",
                      ::GetLastError());
        LH_LOG_ERROR(buf);
        return false;
    }
    out_handle = pipe;
    return true;
}

PipeServer::WaitResult PipeServer::WaitForOverlapped(HANDLE pipe, OVERLAPPED& ov) {
    HANDLE handles[2] = { ov.hEvent, stop_event_ };
    const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
        return WaitResult::Io;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
        // Shutdown requested. Cancel the pending I/O and wait for the
        // cancellation to complete before the caller closes the handle.
        ::CancelIoEx(pipe, &ov);
        DWORD transferred = 0;
        ::GetOverlappedResult(pipe, &ov, &transferred, /*bWait=*/TRUE);
        return WaitResult::Shutdown;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "WaitForMultipleObjects failed: 0x%lx", wait);
    LH_LOG_ERROR(buf);
    return WaitResult::Error;
}

bool PipeServer::ConnectClient(HANDLE pipe) {
    OverlappedOp op;
    if (!op.valid()) {
        LH_LOG_ERROR("ConnectClient: failed to create overlapped event");
        return false;
    }

    if (::ConnectNamedPipe(pipe, op.get())) {
        // Synchronous success — overlapped ConnectNamedPipe should never return
        // TRUE on Windows, but if it does we just succeeded.
        return true;
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        // Client connected between CreateNamedPipe and ConnectNamedPipe; this
        // is a success condition for overlapped pipes too.
        return true;
    }
    if (err != ERROR_IO_PENDING) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "ConnectNamedPipe failed: lastError=%lu", err);
        LH_LOG_WARN(buf);
        return false;
    }

    switch (WaitForOverlapped(pipe, *op.get())) {
        case WaitResult::Io:
            return true;
        case WaitResult::Shutdown:
        case WaitResult::Error:
        default:
            return false;
    }
}

bool PipeServer::ReadExactOverlapped(HANDLE pipe, void* buffer, DWORD count) {
    auto* p = static_cast<std::uint8_t*>(buffer);
    DWORD remaining = count;
    while (remaining > 0) {
        OverlappedOp op;
        if (!op.valid()) {
            LH_LOG_ERROR("ReadExactOverlapped: failed to create event");
            return false;
        }

        DWORD got_sync = 0;
        const BOOL ok = ::ReadFile(pipe, p, remaining, &got_sync, op.get());
        DWORD got = 0;
        if (ok) {
            got = got_sync;
        } else {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (err != ERROR_BROKEN_PIPE) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "ReadFile failed: lastError=%lu", err);
                    LH_LOG_WARN(buf);
                }
                return false;
            }
            switch (WaitForOverlapped(pipe, *op.get())) {
                case WaitResult::Io:
                    if (!::GetOverlappedResult(pipe, op.get(), &got, /*bWait=*/FALSE)) {
                        return false;
                    }
                    break;
                case WaitResult::Shutdown:
                case WaitResult::Error:
                default:
                    return false;
            }
        }
        if (got == 0) {
            return false;
        }
        p += got;
        remaining -= got;
    }
    return true;
}

bool PipeServer::WriteExactOverlapped(HANDLE pipe, const void* buffer, DWORD count) {
    const auto* p = static_cast<const std::uint8_t*>(buffer);
    DWORD remaining = count;
    while (remaining > 0) {
        OverlappedOp op;
        if (!op.valid()) {
            LH_LOG_ERROR("WriteExactOverlapped: failed to create event");
            return false;
        }

        DWORD wrote_sync = 0;
        const BOOL ok = ::WriteFile(pipe, p, remaining, &wrote_sync, op.get());
        DWORD wrote = 0;
        if (ok) {
            wrote = wrote_sync;
        } else {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (err != ERROR_BROKEN_PIPE && err != ERROR_NO_DATA) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "WriteFile failed: lastError=%lu", err);
                    LH_LOG_WARN(buf);
                }
                return false;
            }
            switch (WaitForOverlapped(pipe, *op.get())) {
                case WaitResult::Io:
                    if (!::GetOverlappedResult(pipe, op.get(), &wrote, /*bWait=*/FALSE)) {
                        return false;
                    }
                    break;
                case WaitResult::Shutdown:
                case WaitResult::Error:
                default:
                    return false;
            }
        }
        if (wrote == 0) {
            return false;
        }
        p += wrote;
        remaining -= wrote;
    }
    return true;
}

bool PipeServer::ReadFrame(HANDLE pipe, std::vector<std::uint8_t>& out_payload) {
    std::uint32_t len_le = 0;
    if (!ReadExactOverlapped(pipe, &len_le, sizeof(len_le))) {
        return false;
    }
    if (len_le > kMaxFrameSize) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "ReadFrame: oversized frame: %u bytes (cap=%u)",
                      len_le, kMaxFrameSize);
        LH_LOG_ERROR(buf);
        return false;
    }
    out_payload.resize(len_le);
    if (len_le == 0) {
        return true;
    }
    return ReadExactOverlapped(pipe, out_payload.data(), len_le);
}

bool PipeServer::WriteFrame(HANDLE pipe, const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaxFrameSize) {
        return false;
    }
    const std::uint32_t len_le = static_cast<std::uint32_t>(payload.size());
    // Serialize all writes onto active_pipe_. Both the accept loop's reply
    // path (ServeOneClient) and external threads pushing unsolicited frames
    // (SendFrame) funnel through here, so the lock is the single point that
    // prevents byte-level interleaving of distinct frames.
    std::lock_guard<std::mutex> lock(write_mu_);
    if (!WriteExactOverlapped(pipe, &len_le, sizeof(len_le))) {
        return false;
    }
    if (len_le == 0) {
        return true;
    }
    return WriteExactOverlapped(pipe, payload.data(), len_le);
}

bool PipeServer::ServeOneClient(HANDLE pipe) {
    LH_LOG_INFO("Pipe client connected");

    // Publish the live pipe so SendFrame and the writer thread can route
    // outbound frames here. The handle is owned by Run() and remains valid
    // until we return from this function and Run() disconnects/closes it.
    {
        std::lock_guard<std::mutex> lock(active_pipe_mu_);
        active_pipe_ = pipe;
    }
    auto clear_active = [this]() {
        std::lock_guard<std::mutex> lock(active_pipe_mu_);
        active_pipe_ = nullptr;
    };

    while (!stop_.load()) {
        std::vector<std::uint8_t> req;
        if (!ReadFrame(pipe, req)) {
            clear_active();
            return false;
        }

        std::vector<std::uint8_t> reply;
        if (handler_) {
            reply = handler_(req);
        }

        if (!reply.empty()) {
            // Hand the reply to the writer thread instead of writing
            // inline. The writer thread serializes all outbound bytes
            // FIFO, so this reply still appears before any unsolicited
            // frame the handler may have enqueued.
            (void)EnqueueOutbound(std::move(reply));
        }
    }
    clear_active();
    return true;
}

bool PipeServer::EnqueueOutbound(std::vector<std::uint8_t> payload) {
    if (stop_.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(outbound_mu_);
        outbound_.push_back(std::move(payload));
    }
    outbound_cv_.notify_one();
    return true;
}

bool PipeServer::SendFrame(const std::vector<std::uint8_t>& payload) {
    // Return false if there is no current client so callers can log the
    // drop. There's a benign race where active_pipe_ becomes null between
    // this check and the writer thread's pop — in that case the frame is
    // queued, the writer sees no pipe, and the frame is dropped silently.
    {
        std::lock_guard<std::mutex> lock(active_pipe_mu_);
        if (active_pipe_ == nullptr) {
            return false;
        }
    }
    return EnqueueOutbound(payload);
}

void PipeServer::WriterThreadMain() {
    while (true) {
        std::vector<std::uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(outbound_mu_);
            outbound_cv_.wait(lock, [this] {
                return stop_.load() || !outbound_.empty();
            });
            if (outbound_.empty()) {
                // stop_ is set and the queue drained — exit cleanly so
                // PipeServer::Run can join us.
                return;
            }
            frame = std::move(outbound_.front());
            outbound_.pop_front();
        }

        HANDLE pipe = nullptr;
        {
            std::lock_guard<std::mutex> lock(active_pipe_mu_);
            pipe = active_pipe_;
        }
        if (pipe == nullptr) {
            // No client — drop. Subsequent SendFrame calls will return
            // false at the active_pipe_ check, so a build-up of dropped
            // frames during reconnect windows is bounded.
            continue;
        }
        // WriteFrame internally grabs write_mu_ + does the overlapped
        // WriteFile with stop_event_ cancellation. Failure usually means
        // the client disconnected; ServeOneClient will detect that
        // independently via its own ReadFrame failing.
        (void)WriteFrame(pipe, frame);
    }
}

void PipeServer::Run() {
    if (stop_event_ == nullptr) {
        LH_LOG_ERROR("PipeServer::Run aborted — stop_event_ was not initialized");
        return;
    }

    // Spin up the writer thread before the accept loop so the very first
    // on-connect frame (and any unsolicited frame that follows) has a
    // consumer for the outbound queue.
    writer_thread_ = std::thread([this] { WriterThreadMain(); });

    while (!stop_.load()) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        if (!CreatePipe(pipe)) {
            // Avoid a tight retry loop, but stay responsive to Stop().
            ::WaitForSingleObject(stop_event_, 1000);
            continue;
        }

        LH_LOG_INFO("Waiting for pipe client to connect");
        if (!ConnectClient(pipe)) {
            ::CloseHandle(pipe);
            continue;
        }

        // Publish active_pipe_ before the on-connect frame so the writer
        // thread can route the READY handshake (and any subsequent
        // unsolicited frame) to the new client.
        {
            std::lock_guard<std::mutex> lock(active_pipe_mu_);
            active_pipe_ = pipe;
        }

        // Push the on-connect frame (e.g. READY handshake) before we start
        // pulling requests. Enqueue onto the writer thread so the byte
        // stream stays FIFO with anything else the dispatcher publishes.
        if (on_connect_) {
            auto initial = on_connect_();
            if (!initial.empty()) {
                (void)EnqueueOutbound(std::move(initial));
            }
        }

        ServeOneClient(pipe);

        // ServeOneClient clears active_pipe_ on exit, but be explicit here
        // in case it returned via an unusual path. Without this, the
        // writer thread could try to write to a closed handle.
        {
            std::lock_guard<std::mutex> lock(active_pipe_mu_);
            active_pipe_ = nullptr;
        }

        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        LH_LOG_INFO("Pipe client disconnected; ready for next connection");
    }
    LH_LOG_INFO("PipeServer::Run exiting (stop requested)");

    // Wake the writer thread so it sees stop_ and exits after draining.
    outbound_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

}  // namespace leviathan::clipboard_helper
