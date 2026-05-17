#include "socket_server.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <utility>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

// Encode a 4-byte little-endian length prefix.
void WriteLengthLE(std::uint8_t out[4], std::uint32_t len) {
    out[0] = static_cast<std::uint8_t>(len & 0xFF);
    out[1] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((len >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((len >> 24) & 0xFF);
}

std::uint32_t ReadLengthLE(const std::uint8_t in[4]) {
    return static_cast<std::uint32_t>(in[0])
         | (static_cast<std::uint32_t>(in[1]) << 8)
         | (static_cast<std::uint32_t>(in[2]) << 16)
         | (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace

SocketServer::SocketServer() = default;

SocketServer::~SocketServer() {
    Stop();
}

bool SocketServer::Start(const std::string& socket_path,
                         FrameHandler        handler,
                         OnConnectFn         on_connect,
                         OnDisconnectFn      on_disconnect) {
    socket_path_   = socket_path;
    handler_       = std::move(handler);
    on_connect_    = std::move(on_connect);
    on_disconnect_ = std::move(on_disconnect);

    // Best-effort cleanup of a stale socket file from a previous run that
    // crashed before unlinking. ENOENT is fine (path didn't exist).
    if (::unlink(socket_path_.c_str()) != 0 && errno != ENOENT) {
        std::ostringstream m;
        m << "Failed to unlink stale socket '" << socket_path_
          << "': " << std::strerror(errno);
        LH_LOG_WARN(m.str());
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::ostringstream m;
        m << "socket(AF_UNIX) failed: " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        std::ostringstream m;
        m << "Socket path too long (" << socket_path_.size()
          << " >= " << sizeof(addr.sun_path) << "): '" << socket_path_ << "'";
        LH_LOG_ERROR(m.str());
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // umask 0o077 before bind so the resulting socket node is mode 0o700;
    // then explicit chmod 0o600 for read+write owner only. The umask
    // dance covers the brief window before chmod runs.
    const mode_t prev_umask = ::umask(0077);
    const int    bind_rc    = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::umask(prev_umask);

    if (bind_rc != 0) {
        std::ostringstream m;
        m << "bind('" << socket_path_ << "') failed: " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        ::close(fd);
        return false;
    }

    if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::ostringstream m;
        m << "chmod 0600 on '" << socket_path_ << "' failed: " << std::strerror(errno);
        LH_LOG_WARN(m.str());
        // Continue — the umask path above already restricted access; chmod
        // is belt-and-braces. Production should still surface the warning.
    }

    if (::listen(fd, 1) != 0) {
        std::ostringstream m;
        m << "listen() failed: " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        ::close(fd);
        ::unlink(socket_path_.c_str());
        return false;
    }

    listen_fd_.store(fd, std::memory_order_release);
    worker_ = std::thread([this] { WorkerLoop(); });

    std::ostringstream m;
    m << "SocketServer listening at '" << socket_path_ << "' (fd=" << fd << ")";
    LH_LOG_INFO(m.str());
    return true;
}

void SocketServer::Stop() {
    if (stop_.exchange(true)) {
        // Already stopping — wait for worker to finish (idempotent).
        if (worker_.joinable()) worker_.join();
        return;
    }

    // Wake any blocking accept() / read() / write(). shutdown(2) on the
    // listening socket makes accept return EINVAL; on the client socket
    // it returns 0 from a blocked read. close(2) finalises the fd
    // numbers. The worker observes stop_ on its next loop iteration.
    const int lfd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    if (lfd >= 0) {
        ::shutdown(lfd, SHUT_RDWR);
        ::close(lfd);
    }
    {
        std::lock_guard<std::mutex> g(client_mutex_);
        if (client_fd_ >= 0) {
            ::shutdown(client_fd_, SHUT_RDWR);
            // Don't close from here — the worker thread owns the fd's
            // close to avoid double-close races. shutdown is enough to
            // unblock any pending read/write.
        }
    }

    if (worker_.joinable()) worker_.join();

    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

bool SocketServer::SendFrame(const std::vector<std::uint8_t>& payload) {
    std::lock_guard<std::mutex> g(client_mutex_);
    if (client_fd_ < 0) return false;
    return WriteFrame(client_fd_, payload.data(), payload.size());
}

void SocketServer::WorkerLoop() {
    const int lfd = listen_fd_.load(std::memory_order_acquire);
    if (lfd < 0) {
        if (on_disconnect_) on_disconnect_();
        stopped_.store(true, std::memory_order_release);
        return;
    }

    bool fire_disconnect = true;
    while (!stop_.load(std::memory_order_acquire)) {
        sockaddr_un peer{};
        socklen_t   peer_len = sizeof(peer);
        const int   cfd      = ::accept4(lfd, reinterpret_cast<sockaddr*>(&peer),
                                         &peer_len, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (stop_.load(std::memory_order_acquire)) {
                // Stop() called — caller already knows to tear down,
                // no need to fire on_disconnect from here.
                fire_disconnect = false;
                break;
            }
            // Any other accept failure (EMFILE, ECONNABORTED, …) on
            // the single-client model is unrecoverable: parent will
            // observe helper exit and restart with a fresh socket.
            std::ostringstream m;
            m << "accept() failed: " << std::strerror(errno);
            LH_LOG_ERROR(m.str());
            break;
        }

        {
            std::lock_guard<std::mutex> g(client_mutex_);
            client_fd_ = cfd;
        }
        LH_LOG_INFO("Client connected");

        if (on_connect_) {
            auto greet = on_connect_();
            if (!greet.empty()) {
                std::lock_guard<std::mutex> g(client_mutex_);
                if (client_fd_ >= 0) {
                    WriteFrame(client_fd_, greet.data(), greet.size());
                }
            }
        }

        std::vector<std::uint8_t> frame;
        while (!stop_.load(std::memory_order_acquire)) {
            if (!ReadFrame(cfd, frame)) {
                break;  // EOF, error, or oversized frame
            }
            if (handler_) {
                auto reply = handler_(frame);
                if (!reply.empty()) {
                    std::lock_guard<std::mutex> g(client_mutex_);
                    if (client_fd_ >= 0 &&
                        !WriteFrame(client_fd_, reply.data(), reply.size())) {
                        break;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> g(client_mutex_);
            client_fd_ = -1;
        }
        ::close(cfd);
        LH_LOG_INFO("Client disconnected");

        // Single-client model: after one parent disconnects we exit
        // the accept loop too, matching macOS. The parent (shen /
        // leviathan) is the only one that should ever connect; if it
        // crashed and respawns it will start a fresh helper anyway.
        break;
    }

    // Fire on_disconnect outside the loop so it runs on EVERY path
    // that ends the session — including accept() failures BEFORE any
    // client connected. Without this, an early accept error would
    // exit the worker thread but leave main blocked in app.exec()
    // forever (the "zombie helper" bug gemini caught).
    if (fire_disconnect && on_disconnect_) on_disconnect_();

    stopped_.store(true, std::memory_order_release);
}

bool SocketServer::ReadExact(int fd, void* buf, std::size_t len) {
    auto*       p   = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::read(fd, p + got, len - got);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;  // EOF
        }
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool SocketServer::WriteExact(int fd, const void* buf, std::size_t len) {
    const auto* p    = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool SocketServer::ReadFrame(int fd, std::vector<std::uint8_t>& out) {
    std::uint8_t header[4];
    if (!ReadExact(fd, header, sizeof(header))) return false;
    const std::uint32_t len = ReadLengthLE(header);
    if (len > kMaxFrameSize) {
        std::ostringstream m;
        m << "Inbound frame size " << len
          << " exceeds cap " << kMaxFrameSize << "; closing connection";
        LH_LOG_ERROR(m.str());
        return false;
    }
    // resize(len) can throw bad_alloc for ~16 MiB frames under memory
    // pressure. Treat that the same as any other read failure: tear
    // down the session cleanly instead of unwinding through the worker
    // thread (which would skip on_disconnect, zombie main).
    try {
        out.resize(len);
    } catch (const std::bad_alloc&) {
        std::ostringstream m;
        m << "Failed to allocate " << len << " bytes for inbound frame";
        LH_LOG_ERROR(m.str());
        return false;
    }
    if (len == 0) return true;
    return ReadExact(fd, out.data(), len);
}

bool SocketServer::WriteFrame(int fd, const std::uint8_t* data, std::size_t len) {
    if (len > kMaxFrameSize) {
        std::ostringstream m;
        m << "Refusing to send oversized frame (" << len
          << " bytes, cap " << kMaxFrameSize << ")";
        LH_LOG_ERROR(m.str());
        return false;
    }
    std::uint8_t header[4];
    WriteLengthLE(header, static_cast<std::uint32_t>(len));
    if (!WriteExact(fd, header, sizeof(header))) return false;
    if (len == 0) return true;
    return WriteExact(fd, data, len);
}

}  // namespace leviathan::clipboard_helper
