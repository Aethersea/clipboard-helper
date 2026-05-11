#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace leviathan::clipboard_helper {

// MessageHandler receives one complete length-prefixed message payload and may
// return a response payload to send back. Returning an empty vector means no
// reply for this message.
using MessageHandler = std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)>;

class PipeServer {
public:
    PipeServer(std::wstring pipe_name, MessageHandler handler);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Runs the accept→serve loop until Stop() is called or the parent disappears.
    // Single-client at a time; reconnects after each disconnect.
    void Run();

    // Asks the server loop to wind down. Safe to call from any thread (e.g. the
    // console Ctrl handler or the parent-watchdog). Implemented by signaling a
    // manual-reset shutdown event that the overlapped wait loops also block on,
    // so blocking I/O is canceled cooperatively rather than left hanging.
    void Stop();

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

    std::wstring        pipe_name_;
    MessageHandler      handler_;
    std::atomic<bool>   stop_{false};

    // Manual-reset event. CreateEvent failure leaves this NULL; Run() checks.
    HANDLE              stop_event_{nullptr};
};

}  // namespace leviathan::clipboard_helper
