#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace leviathan::clipboard_helper {

// MessageHandler receives one complete length-prefixed message payload and may
// return a response payload to send back. Returning an empty vector means no
// reply for this message.
using MessageHandler = std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)>;

// OnConnectHandler is invoked once each time a client connects, before any
// request is read. It returns an optional frame to push to the client (e.g. a
// HELPER_MESSAGE_TYPE_READY handshake). Return an empty vector to send nothing.
using OnConnectHandler = std::function<std::vector<std::uint8_t>()>;

class PipeServer {
public:
    PipeServer(std::wstring pipe_name, MessageHandler handler);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Optionally install a handler that produces a frame to push immediately
    // after each client connects (before reading the first request).
    void SetOnConnect(OnConnectHandler on_connect);

    // Runs the accept→serve loop until Stop() is called or the parent disappears.
    // Single-client at a time; reconnects after each disconnect.
    void Run();

    // Asks the server loop to wind down. Safe to call from any thread (e.g. the
    // console Ctrl handler or the parent-watchdog). Implemented by signaling a
    // manual-reset shutdown event that the overlapped wait loops also block on,
    // so blocking I/O is canceled cooperatively rather than left hanging.
    void Stop();

    // SendFrame enqueues a length-prefixed payload for transmission on the
    // currently connected client pipe. Safe to call from any thread,
    // including the STA worker on its own message-pump thread: the actual
    // WriteFile happens on a dedicated writer thread so callers never block
    // on pipe back-pressure.
    //
    // Returns false only when there is no active client at enqueue time or
    // when the server is already stopping; a successful return means the
    // frame is queued, NOT that the parent has received it. Frames are
    // emitted to the wire in FIFO order so request replies stay paired with
    // their requests even when unsolicited frames are pushed concurrently.
    bool SendFrame(const std::vector<std::uint8_t>& payload);

private:
    enum class WaitResult {
        Io,        // overlapped operation completed
        Shutdown,  // stop_event_ was signaled
        Error,     // WaitForMultipleObjects failed
    };

    bool CreatePipe(HANDLE& out_handle);
    bool ServeOneClient(HANDLE pipe);
    bool ConnectClient(HANDLE pipe);
    bool ReadFrame(HANDLE pipe, std::vector<std::uint8_t>& out_payload);
    bool WriteFrame(HANDLE pipe, const std::vector<std::uint8_t>& payload);
    bool ReadExactOverlapped(HANDLE pipe, void* buffer, DWORD count);
    bool WriteExactOverlapped(HANDLE pipe, const void* buffer, DWORD count);

    // Waits for the overlapped I/O to complete or for stop_event_ to be set.
    // On Shutdown, cancels the I/O and waits for the cancellation to settle so
    // the caller can safely close the handle.
    WaitResult WaitForOverlapped(HANDLE pipe, OVERLAPPED& ov);

    // Body of the writer thread. Pops frames from outbound_ and writes them
    // to active_pipe_ inline (using the existing overlapped + stop-event
    // dance). Exits when stop_ is set and outbound_ drains.
    void WriterThreadMain();

    // Pushes a frame onto outbound_ and wakes the writer thread. Returns
    // false when stop_ is already set. Internal helper shared by SendFrame
    // and ServeOneClient's reply path.
    bool EnqueueOutbound(std::vector<std::uint8_t> payload);

    std::wstring        pipe_name_;
    MessageHandler      handler_;
    OnConnectHandler    on_connect_;
    std::atomic<bool>   stop_{false};

    // Manual-reset event. CreateEvent failure leaves this NULL; Run() checks.
    HANDLE              stop_event_{nullptr};

    // Currently-active client pipe. Set inside ServeOneClient, cleared on
    // disconnect. Read by SendFrame via active_pipe_mu_ + write_mu_.
    HANDLE              active_pipe_{nullptr};
    std::mutex          active_pipe_mu_;
    // Serializes all WriteFile calls onto active_pipe_. Held only by the
    // writer thread (and the synchronous on-connect path inside Run()
    // before active_pipe_ is published) so a frame is never split mid-bytes.
    std::mutex          write_mu_;

    // Outbound frame queue + writer thread. Decoupling pipe IO from request
    // handlers prevents the deadlock where the STA worker blocks inside
    // WriteFile waiting for the parent to drain, while the parent is itself
    // waiting for a synchronous reply that needs the STA worker. Every
    // unsolicited frame (CLIPBOARD_CHANGED, DATA_REQUEST, FILE_CHUNK_DATA)
    // and every request reply funnels through here.
    std::deque<std::vector<std::uint8_t>> outbound_;
    std::mutex                            outbound_mu_;
    std::condition_variable               outbound_cv_;
    std::thread                           writer_thread_;
};

}  // namespace leviathan::clipboard_helper
