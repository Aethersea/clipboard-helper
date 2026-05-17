// socket_server tests — exercise the AF_UNIX listener and length-
// prefixed framing end-to-end with a real Unix socket pair. Each test
// uses a per-test socket path under $TMPDIR / /tmp so parallel test
// runs don't collide.

#include "socket_server.h"
#include "test_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using leviathan::clipboard_helper::SocketServer;

namespace {

std::string MakeSocketPath(const char* tag) {
    // /tmp is reliably world-writable on every distro and uses tmpfs in
    // CI, so unlink-and-recreate is fast. We embed the test PID and a
    // tag so parallel ctest runs don't collide.
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/clipboard-helper-test-%d-%s.sock",
                  static_cast<int>(::getpid()), tag);
    return std::string(buf);
}

// Connect a client socket to `path` and return the fd. Returns -1 on
// failure with errno preserved.
int ClientConnect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

// Retry connect a few times — Start() returns once the listener fd is
// installed, but accept() races with the worker thread spinning up.
int ClientConnectWithRetry(const std::string& path,
                           std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ClientConnect(path);
        if (fd >= 0) return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool ReadAll(int fd, std::uint8_t* data, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::read(fd, data + got, len - got);
        if (n > 0) { got += static_cast<std::size_t>(n); continue; }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

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

// Synchronously read one full frame from `fd`. Returns true on success.
bool ReadOneFrame(int fd, std::vector<std::uint8_t>& out) {
    std::uint8_t header[4];
    if (!ReadAll(fd, header, sizeof(header))) return false;
    const std::uint32_t len = ReadLengthLE(header);
    out.resize(len);
    if (len == 0) return true;
    return ReadAll(fd, out.data(), len);
}

}  // namespace

// ─── Start / Stop lifecycle ───────────────────────────────────────────────

TEST(SocketServer, StartCreatesSocketFile) {
    const auto path = MakeSocketPath("start-creates");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        []() { return std::vector<std::uint8_t>{}; }));
    // Listener fd is up; client can connect.
    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    EXPECT_GE(cfd, 0);
    if (cfd >= 0) ::close(cfd);
    s.Stop();
}

TEST(SocketServer, StopUnlinksSocketFile) {
    const auto path = MakeSocketPath("stop-unlinks");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        []() { return std::vector<std::uint8_t>{}; }));
    s.Stop();
    EXPECT_NE(::access(path.c_str(), F_OK), 0);
}

TEST(SocketServer, StopIsIdempotent) {
    const auto path = MakeSocketPath("stop-idempotent");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        []() { return std::vector<std::uint8_t>{}; }));
    s.Stop();
    s.Stop();  // must not hang / crash
    EXPECT_TRUE(s.IsStopped());
}

// ─── OnConnect ────────────────────────────────────────────────────────────

TEST(SocketServer, OnConnectGreetingDeliveredAsFirstFrame) {
    const auto path = MakeSocketPath("on-connect");
    SocketServer s;
    const std::vector<std::uint8_t> greet = {0xAA, 0xBB, 0xCC};
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        [greet]() { return greet; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(ReadOneFrame(cfd, frame));
    ASSERT_EQ(frame.size(), greet.size());
    for (std::size_t i = 0; i < greet.size(); ++i) {
        EXPECT_EQ(static_cast<int>(frame[i]), static_cast<int>(greet[i]));
    }
    ::close(cfd);
    s.Stop();
}

// ─── Request/Reply framing ────────────────────────────────────────────────

TEST(SocketServer, HandlerEchoesFrame) {
    const auto path = MakeSocketPath("handler-echo");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>& in) { return in; },
                        []() { return std::vector<std::uint8_t>{}; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    const std::uint8_t payload[] = {1, 2, 3, 4, 5};
    std::uint8_t       header[4];
    WriteLengthLE(header, sizeof(payload));
    ASSERT_TRUE(WriteAll(cfd, header, sizeof(header)));
    ASSERT_TRUE(WriteAll(cfd, payload, sizeof(payload)));

    std::vector<std::uint8_t> reply;
    ASSERT_TRUE(ReadOneFrame(cfd, reply));
    ASSERT_EQ(reply.size(), sizeof(payload));
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        EXPECT_EQ(static_cast<int>(reply[i]), static_cast<int>(payload[i]));
    }
    ::close(cfd);
    s.Stop();
}

TEST(SocketServer, HandlerEmptyReplyMeansNoReply) {
    const auto path = MakeSocketPath("handler-noreply");
    SocketServer s;
    std::atomic<int> handler_calls{0};
    ASSERT_TRUE(s.Start(path,
                        [&handler_calls](const std::vector<std::uint8_t>&) {
                            handler_calls.fetch_add(1);
                            return std::vector<std::uint8_t>{};
                        },
                        []() { return std::vector<std::uint8_t>{}; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    const std::uint8_t header[4] = {0, 0, 0, 0};  // zero-length frame
    ASSERT_TRUE(WriteAll(cfd, header, sizeof(header)));

    // Give the worker a moment to see the frame and decide there's no
    // reply. (No bytes back so we can't synchronise on a reply.)
    for (int i = 0; i < 100 && handler_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(handler_calls.load(), 1);

    // The connection should still be open — closing from the client
    // side now exercises the worker's read-EOF path.
    ::close(cfd);
    s.Stop();
}

// ─── Asynchronous SendFrame ───────────────────────────────────────────────

TEST(SocketServer, SendFrameFromExternalThread) {
    const auto path = MakeSocketPath("send-frame");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        []() { return std::vector<std::uint8_t>{}; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    // SendFrame is allowed once the worker has set client_fd_. There's
    // a tiny race window between accept returning and the worker
    // assigning client_fd_; retry a few times.
    const std::vector<std::uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    bool sent = false;
    for (int i = 0; i < 100 && !sent; ++i) {
        sent = s.SendFrame(payload);
        if (!sent) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(sent);

    std::vector<std::uint8_t> got;
    ASSERT_TRUE(ReadOneFrame(cfd, got));
    ASSERT_EQ(got.size(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(static_cast<int>(got[i]), static_cast<int>(payload[i]));
    }
    ::close(cfd);
    s.Stop();
}

// ─── Oversized frame rejection ────────────────────────────────────────────

TEST(SocketServer, OversizedInboundFrameClosesConnection) {
    const auto path = MakeSocketPath("oversized");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; },
                        []() { return std::vector<std::uint8_t>{}; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    // Advertise a frame just over the cap. The worker must abort the
    // session without reading the body (which we never send anyway).
    std::uint8_t header[4];
    WriteLengthLE(header, SocketServer::kMaxFrameSize + 1);
    ASSERT_TRUE(WriteAll(cfd, header, sizeof(header)));

    // Subsequent reads from the client should observe EOF as the
    // worker closes the socket. Wait up to 2s.
    char     dummy[16];
    ssize_t  n  = 0;
    bool     eof = false;
    for (int i = 0; i < 200; ++i) {
        n = ::read(cfd, dummy, sizeof(dummy));
        if (n == 0) { eof = true; break; }
        if (n < 0 && errno != EINTR && errno != EAGAIN) { eof = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(eof);
    ::close(cfd);
    s.Stop();
}

// ─── Split-frame reassembly ───────────────────────────────────────────────

TEST(SocketServer, SplitFrameReassembledByReadExact) {
    const auto path = MakeSocketPath("split-frame");
    SocketServer s;
    ASSERT_TRUE(s.Start(path,
                        [](const std::vector<std::uint8_t>& in) { return in; },
                        []() { return std::vector<std::uint8_t>{}; }));

    const int cfd = ClientConnectWithRetry(path, std::chrono::seconds(2));
    ASSERT_TRUE(cfd >= 0);

    // Send the length prefix one byte at a time, then the body in two
    // halves, with small sleeps between each chunk. ReadExact must
    // reassemble correctly.
    const std::uint8_t payload[] = {'h','e','l','l','o','w','o','r','l','d'};
    std::uint8_t       header[4];
    WriteLengthLE(header, sizeof(payload));

    for (std::size_t i = 0; i < sizeof(header); ++i) {
        ASSERT_TRUE(WriteAll(cfd, header + i, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(WriteAll(cfd, payload, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(WriteAll(cfd, payload + 4, sizeof(payload) - 4));

    std::vector<std::uint8_t> reply;
    ASSERT_TRUE(ReadOneFrame(cfd, reply));
    ASSERT_EQ(reply.size(), sizeof(payload));
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        EXPECT_EQ(static_cast<int>(reply[i]), static_cast<int>(payload[i]));
    }
    ::close(cfd);
    s.Stop();
}
