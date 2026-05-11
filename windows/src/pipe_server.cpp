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

    while (!stop_.load()) {
        std::vector<std::uint8_t> req;
        if (!ReadFrame(pipe, req)) {
            return false;
        }

        std::vector<std::uint8_t> reply;
        if (handler_) {
            reply = handler_(req);
        }

        if (!reply.empty()) {
            if (!WriteFrame(pipe, reply)) {
                return false;
            }
        }
    }
    return true;
}

void PipeServer::Run() {
    if (stop_event_ == nullptr) {
        LH_LOG_ERROR("PipeServer::Run aborted — stop_event_ was not initialized");
        return;
    }

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

        // Push the on-connect frame (e.g. READY handshake) before we start
        // pulling requests. A failure here is fatal for this client only.
        if (on_connect_) {
            auto initial = on_connect_();
            if (!initial.empty()) {
                if (!WriteFrame(pipe, initial)) {
                    LH_LOG_WARN("on-connect frame write failed; dropping client");
                    ::FlushFileBuffers(pipe);
                    ::DisconnectNamedPipe(pipe);
                    ::CloseHandle(pipe);
                    continue;
                }
            }
        }

        ServeOneClient(pipe);

        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        LH_LOG_INFO("Pipe client disconnected; ready for next connection");
    }
    LH_LOG_INFO("PipeServer::Run exiting (stop requested)");
}

}  // namespace leviathan::clipboard_helper
