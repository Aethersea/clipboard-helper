#pragma once
//
// dispatch — routes inbound HelperMessage frames to the ClipboardManager
// and pushes outbound events (CLIPBOARD_CHANGED, DATA_REQUEST) back to
// the parent via the SocketServer.
//
// The Dispatcher does NOT own the ClipboardManager or SocketServer —
// main.cpp constructs all three and wires them together. Lifetimes:
// Dispatcher must outlive the ClipboardManager callbacks it installs
// and any in-flight frame handling. main.cpp's Stop() ordering is:
//
//     watchdog.Stop();        // first: no new shutdown triggers
//     socket_server.Stop();   // drain inbound; waits for handler
//     dispatcher.Detach();    // clear ClipboardManager callbacks
//     // destructors run in reverse order

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace leviathan::clipboard_helper {

class ClipboardManager;
class SocketServer;

class Dispatcher {
public:
    Dispatcher(ClipboardManager* clipboard,
               SocketServer*     socket,
               std::function<void()> on_shutdown_request);
    ~Dispatcher();

    Dispatcher(const Dispatcher&)            = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Install callbacks on the ClipboardManager so OS-clipboard changes
    // and OS-paste triggers turn into CLIPBOARD_CHANGED / DATA_REQUEST
    // frames pushed via the SocketServer.
    void Attach();

    // Reverse Attach(). Detaches our callbacks from the ClipboardManager
    // and stops responding to inbound frames. Safe to call once; idempotent.
    void Detach();

    // SocketServer::FrameHandler — invoked for every inbound frame.
    // Returns any synchronous reply bytes (e.g. CLIPBOARD_CONTENT for
    // GET_CLIPBOARD); empty vector means no immediate reply (events
    // like CLIPBOARD_CHANGED are pushed asynchronously via Attach()).
    std::vector<std::uint8_t> Handle(const std::vector<std::uint8_t>& frame);

    // SocketServer::OnConnectFn — returns the READY frame to send the
    // moment a client connects.
    std::vector<std::uint8_t> OnConnect();

private:
    std::vector<std::uint8_t> HandleSetClipboard(const std::uint8_t* data, std::size_t len);
    std::vector<std::uint8_t> HandleAnnounceDelayed(const std::uint8_t* data, std::size_t len);
    std::vector<std::uint8_t> HandleProvideData(const std::uint8_t* data, std::size_t len);
    std::vector<std::uint8_t> HandleGetClipboard();
    std::vector<std::uint8_t> HandleShutdown();

    ClipboardManager*     clipboard_;
    SocketServer*         socket_;
    std::function<void()> on_shutdown_request_;
    std::atomic<bool>     attached_{false};
};

}  // namespace leviathan::clipboard_helper
