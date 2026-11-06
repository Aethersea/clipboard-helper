#pragma once
//
// frame_pipe — a real SocketServer on a temp AF_UNIX path plus a client fd
// the test reads from. Lets tests observe the frames a Dispatcher pushes via
// SocketServer::SendFrame without mocking the (non-virtual) server.

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "socket_server.h"

namespace pipetest {

class FramePipe {
public:
    FramePipe() {
        static std::atomic<int> counter{0};
        char buf[256];
        std::snprintf(buf, sizeof(buf), "/tmp/ch-framepipe-%d-%d.sock",
                      static_cast<int>(::getpid()), counter.fetch_add(1));
        path_ = buf;
    }

    ~FramePipe() {
        if (fd_ >= 0) ::close(fd_);
        server_.Stop();
    }

    FramePipe(const FramePipe&)            = delete;
    FramePipe& operator=(const FramePipe&) = delete;

    leviathan::clipboard_helper::SocketServer* server() { return &server_; }

    // Start listening, connect a client and consume the READY frame so the
    // server has a connected client (SendFrame is a no-op otherwise).
    bool Start(leviathan::clipboard_helper::FrameHandler handler) {
        const std::vector<std::uint8_t> ready{0x08, 0x0E};  // type = READY
        if (!server_.Start(path_, std::move(handler),
                           [ready]() { return ready; })) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
            if (fd_ >= 0 && ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (fd_ < 0) return false;
        std::vector<std::uint8_t> first;
        return ReadFrame(first, 3000);
    }

    // Read one length-prefixed frame, waiting up to timeout_ms.
    bool ReadFrame(std::vector<std::uint8_t>& out, int timeout_ms) {
        std::uint8_t hdr[4];
        if (!ReadExact(hdr, 4, timeout_ms)) return false;
        const std::uint32_t len = static_cast<std::uint32_t>(hdr[0])
                                | (static_cast<std::uint32_t>(hdr[1]) << 8)
                                | (static_cast<std::uint32_t>(hdr[2]) << 16)
                                | (static_cast<std::uint32_t>(hdr[3]) << 24);
        out.resize(len);
        return len == 0 || ReadExact(out.data(), len, timeout_ms);
    }

    // True iff no frame arrives within `ms`.
    bool NoFrameWithin(int ms) {
        pollfd p{fd_, POLLIN, 0};
        return ::poll(&p, 1, ms) == 0;
    }

private:
    bool ReadExact(std::uint8_t* buf, std::size_t len, int timeout_ms) {
        std::size_t got = 0;
        while (got < len) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) return false;
            const ssize_t n = ::read(fd_, buf + got, len - got);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::string                                path_;
    leviathan::clipboard_helper::SocketServer  server_;
    int                                        fd_{-1};
};

}  // namespace pipetest
