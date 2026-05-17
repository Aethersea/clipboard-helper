#pragma once
//
// socket_server — AF_UNIX SOCK_STREAM listener with length-prefixed
// protobuf framing.
//
// Wire format (same on every platform):
//   [4-byte little-endian length][payload of `length` bytes]
//
// Threading model:
//   - Start() spawns one worker thread that owns the listening fd. The
//     worker accepts at most ONE client connection (matching the
//     macOS/Windows helpers — one parent per helper), then loops reading
//     frames until EOF / error / Stop(). Each parsed frame is handed to
//     the FrameHandler; the handler's return value (if non-empty) is
//     sent back to the client as a single frame.
//   - SendFrame() can be called from any thread (mutex-protected). Used
//     by Dispatcher to push CLIPBOARD_CHANGED, DATA_REQUEST, READY, etc.
//     to the parent outside of the request/response loop.
//   - Stop() shuts down the connected socket and the listening socket;
//     blocking read/write/accept all unblock with EBADF/0, the worker
//     observes the stop_ flag and exits, and join() returns.
//
// Frame size cap: 16 MiB. This matches the Windows helper. Anything
// larger should go through the FILE_CHUNK_REQUEST / FILE_CHUNK_DATA
// streaming path, not a single SET_CLIPBOARD frame.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace leviathan::clipboard_helper {

// Hand a decoded inbound frame to the dispatcher; return any reply frame
// to send back to the client immediately (empty vector → no reply). The
// reply path is for HelperMessage types where the response IS the
// request's natural pair — for unsolicited events the dispatcher uses
// SocketServer::SendFrame instead.
using FrameHandler = std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)>;

// Optional: called once after a client connects, before any inbound
// frames are processed. The returned bytes (if non-empty) are sent as
// the first frame to the client — used to push READY on connect.
using OnConnectFn = std::function<std::vector<std::uint8_t>()>;

// Optional: called from the worker thread when the (single) connected
// client disconnects OR an unrecoverable I/O error tears the session
// down. Mirrors the macOS / single-client model: once the parent goes
// away, the helper has no reason to keep running. The callback is
// expected to post a shutdown request to the main event loop in a
// thread-safe way (e.g. QMetaObject::invokeMethod with
// Qt::QueuedConnection on QCoreApplication::quit).
using OnDisconnectFn = std::function<void()>;

class SocketServer {
public:
    // Maximum inbound frame size we'll accept. Frames larger than this
    // are treated as protocol corruption — connection is closed.
    static constexpr std::size_t kMaxFrameSize = 16 * 1024 * 1024;

    SocketServer();
    ~SocketServer();

    SocketServer(const SocketServer&)            = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    // Start listening at `socket_path` and spawn the worker thread.
    // Returns false on bind/listen failure (errno preserved). The path
    // is created with mode 0600 inside the caller's umask; if a stale
    // socket file already exists at the path it is unlink()-ed first.
    //
    // `on_disconnect` may be empty if the caller is uninterested in
    // session teardown notifications (e.g. tests that only exercise
    // framing). Production wiring (main.cpp) always provides it so
    // client disconnect triggers helper shutdown.
    bool Start(const std::string& socket_path,
               FrameHandler        handler,
               OnConnectFn         on_connect,
               OnDisconnectFn      on_disconnect = {});

    // Send an unsolicited frame to the currently-connected client.
    // No-op (returns false) if no client is connected. Thread-safe.
    bool SendFrame(const std::vector<std::uint8_t>& payload);

    // Request shutdown. Closes the listening + connected sockets,
    // sets stop_, joins the worker.
    //
    // Thread-safety: callable from any thread EXCEPT the worker thread
    // itself (which would deadlock on worker_.join()). The FrameHandler
    // runs on the worker thread — handlers that want to trigger
    // shutdown must post the request via on_disconnect / a queued
    // callback into the main thread, not call Stop() directly.
    void Stop();

    // True after the worker thread has exited (either Stop() or the
    // client disconnected and we returned from the accept loop).
    bool IsStopped() const { return stopped_.load(std::memory_order_acquire); }

private:
    void WorkerLoop();
    bool ReadExact(int fd, void* buf, std::size_t len);
    bool WriteExact(int fd, const void* buf, std::size_t len);
    bool ReadFrame(int fd, std::vector<std::uint8_t>& out);
    bool WriteFrame(int fd, const std::uint8_t* data, std::size_t len);

    std::string    socket_path_;
    FrameHandler   handler_;
    OnConnectFn    on_connect_;
    OnDisconnectFn on_disconnect_;

    std::thread        worker_;
    std::atomic<bool>  stop_{false};
    std::atomic<bool>  stopped_{false};

    // listen_fd_ is owned by the worker thread once Start() returns true
    // and the worker has begun; Stop() shuts it down via a separate
    // syscall (shutdown(2)) which is safe to issue from any thread —
    // the worker observes the resulting accept() failure and exits.
    std::atomic<int>   listen_fd_{-1};

    // client_fd_ is set by the worker after accept(); SendFrame()
    // synchronises on send_mutex_ before writing. The worker resets it
    // to -1 on disconnect / error.
    std::mutex         client_mutex_;
    int                client_fd_{-1};
};

}  // namespace leviathan::clipboard_helper
