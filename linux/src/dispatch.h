#pragma once
//
// dispatch — routes inbound HelperMessage frames to the ClipboardManager
// and pushes outbound events (CLIPBOARD_CHANGED, DATA_REQUEST) back to
// the parent via the SocketServer. For local file copies it also owns
// the file_id → path map that FILE_CHUNK_REQUEST is served from, so the
// FILES wire shape does not depend on which backend is live.
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
#include <mutex>
#include <string>
#include <unordered_map>
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
    std::vector<std::uint8_t> HandleFileChunkRequest(const std::uint8_t* data, std::size_t len);

    // Backend callbacks (installed by Attach). Run on whichever thread
    // the backend reports from — the Qt main thread for X11, a detached
    // receive worker for the Wayland backends.
    void OnLocalTextChanged(const std::string& utf8);
    void OnLocalFilesChanged(const std::vector<std::string>& abs_paths);

    // One file of the most recently reported local FILES change. The
    // path comes only from our own clipboard observation — never from a
    // FILE_CHUNK_REQUEST — so the parent can read exactly the files the
    // user copied and nothing else.
    struct LocalFile {
        std::string abs_path;
        bool        is_directory{false};
    };

    ClipboardManager*     clipboard_;
    SocketServer*         socket_;
    std::function<void()> on_shutdown_request_;
    std::atomic<bool>     attached_{false};

    // Serialises "replace the file map + push CLIPBOARD_CHANGED" across
    // backend threads so the map always describes the LAST frame the
    // parent received (two Wayland receive workers can finish in either
    // order). Never taken on the socket thread: Handle() must not wait
    // behind a SendFrame that is itself waiting for the parent to read.
    std::mutex            report_mu_;

    // Guards local_files_. Taken briefly by the report path (swap) and
    // by HandleFileChunkRequest on the socket thread (lookup); never
    // held across I/O.
    std::mutex            files_mu_;
    std::unordered_map<std::string, LocalFile> local_files_;  // file_id → file
};

}  // namespace leviathan::clipboard_helper
