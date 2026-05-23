// leviathan-clipboard-helper — user-session OLE/clipboard agent (Phase 1 skeleton).
//
// Phase 1 scope:
//   - Accept a single named-pipe client (parent leviathan.exe).
//   - Read length-prefixed frames (uint32 LE size + payload bytes).
//   - Echo the payload back unchanged so the parent can verify framing.
//   - Monitor parent PID; exit when parent dies.
//
// Later phases add protobuf decoding, COM/OLE bridge, file transfer, and
// delayed-rendering IDataObject server.

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dispatch.h"
#include "log.h"
#include "pipe_server.h"
#include "session.h"
#include "sta_worker.h"

namespace {

using leviathan::clipboard_helper::PipeServer;

// Single live PipeServer pointer published for the console-ctrl handler. The
// pointer is cleared *before* the server's destructor runs in wmain so the
// handler never dereferences freed storage; combined with Stop() being safe to
// call after the loop has already exited, the worst case is a no-op.
std::atomic<PipeServer*> g_server{nullptr};

// The dispatcher instance is constructed in wmain after StaWorker + PipeServer
// are wired, and published here so the static PipeServer callbacks can route
// through it. Cleared during teardown.
std::atomic<leviathan::clipboard_helper::Dispatcher*> g_dispatcher{nullptr};

// PipeServer message handler. Forwards to the Dispatcher; if the dispatcher
// is not yet (or no longer) installed, drop the frame silently.
std::vector<std::uint8_t> ProtoMessageHandler(const std::vector<std::uint8_t>& req) {
    auto* d = g_dispatcher.load();
    if (d == nullptr) return {};
    return d->Handle(req);
}

// OnConnect handler — pushes a HELPER_MESSAGE_TYPE_READY frame the moment the
// parent connects so it can observe the helper is alive before issuing any
// request.
std::vector<std::uint8_t> OnConnectReady() {
    LH_LOG_INFO("Sending READY frame to new client");
    auto* d = g_dispatcher.load();
    if (d == nullptr) return {};
    return d->OnConnect();
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            LH_LOG_INFO("Console ctrl event received; stopping server");
            if (auto* s = g_server.load()) {
                s->Stop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// Returns 0 (no parent supplied), or the resolved parent PID parsed from argv.
DWORD ParseParentPid(int argc, wchar_t** argv) {
    constexpr std::wstring_view kFlag = L"--parent-pid=";
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.rfind(kFlag, 0) == 0) {
            try {
                return static_cast<DWORD>(std::stoul(std::wstring(arg.substr(kFlag.size()))));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

// Returns the optional override for the pipe name, or an empty string when
// no --pipe-name= flag was passed. Empty falls back to the session-derived
// default. Used so multiple parents (leviathan + shen) can coexist in the
// same Windows session without colliding on the default pipe name.
std::wstring ParsePipeNameOverride(int argc, wchar_t** argv) {
    constexpr std::wstring_view kFlag = L"--pipe-name=";
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.rfind(kFlag, 0) == 0) {
            return std::wstring(arg.substr(kFlag.size()));
        }
    }
    return {};
}

// Returns the inherited parent-process handle parsed from --parent-handle=,
// or nullptr when absent.
//
// The parent duplicates a SYNCHRONIZE-capable, *inheritable* handle to itself
// and passes the numeric value here; we wait on it directly. This is the only
// reliable parent-death signal when the parent runs as NT AUTHORITY\SYSTEM and
// the helper runs as a user-token process in another session: OpenProcess on
// the SYSTEM parent is then denied (ERROR_ACCESS_DENIED), an OpenProcess-based
// watchdog stays permanently disabled, and the helper orphans — it keeps the
// FILE_FLAG_FIRST_PIPE_INSTANCE pipe so every future helper fails
// CreateNamedPipeW with ACCESS_DENIED forever. An inherited handle sidesteps
// the parent's DACL entirely (the access was granted by the parent, which has
// full rights to itself, at duplication time), so the wait works regardless of
// our token or session.
HANDLE ParseParentHandle(int argc, wchar_t** argv) {
    constexpr std::wstring_view kFlag = L"--parent-handle=";
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg.rfind(kFlag, 0) == 0) {
            try {
                // Parse as 64-bit so a full pointer-width handle round-trips.
                const unsigned long long v =
                    std::stoull(std::wstring(arg.substr(kFlag.size())));
                return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(v));
            } catch (...) {
                return nullptr;
            }
        }
    }
    return nullptr;
}

// Watchdog state shared between wmain and the watchdog thread. The cancel
// event lets wmain unblock the thread cleanly so it can be joined.
struct Watchdog {
    DWORD       parent_pid{0};
    HANDLE      parent_handle{nullptr};
    HANDLE      cancel_event{nullptr};
    PipeServer* server{nullptr};
};

void RunParentWatchdog(Watchdog* w) {
    if (w->parent_handle == nullptr) {
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Watching parent PID %lu", w->parent_pid);
    LH_LOG_INFO(buf);

    HANDLE handles[2] = { w->parent_handle, w->cancel_event };
    const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    if (wait == WAIT_OBJECT_0) {
        LH_LOG_INFO("Parent process exited; stopping helper");
        if (w->server) {
            w->server->Stop();
        }
    } else if (wait == WAIT_OBJECT_0 + 1) {
        LH_LOG_INFO("Watchdog cancellation event signaled; exiting watchdog");
    } else {
        char errbuf[64];
        std::snprintf(errbuf, sizeof(errbuf), "Watchdog wait failed: 0x%lx", wait);
        LH_LOG_ERROR(errbuf);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const DWORD session_id = leviathan::clipboard_helper::GetCurrentSessionId();
    std::wstring pipe_name = ParsePipeNameOverride(argc, argv);
    const bool pipe_name_overridden = !pipe_name.empty();
    if (!pipe_name_overridden) {
        pipe_name = leviathan::clipboard_helper::DefaultPipeName(session_id);
    }

    char banner[256];
    std::snprintf(banner, sizeof(banner),
                  "leviathan-clipboard-helper starting (session=%lu, pipe=%s)",
                  session_id,
                  pipe_name_overridden ? "override" : "default");
    LH_LOG_INFO(banner);

    // STA worker hosts OLE clipboard ops + delayed rendering + clipboard
    // change notifications. Must be initialized before the Dispatcher so the
    // dispatcher can hand off WriteClipboard / ReadClipboard work synchronously.
    leviathan::clipboard_helper::StaWorker sta;
    if (!sta.Start()) {
        LH_LOG_ERROR("StaWorker failed to initialize; exiting");
        return 1;
    }

    PipeServer server(pipe_name, &ProtoMessageHandler);
    server.SetOnConnect(&OnConnectReady);
    g_server.store(&server);

    leviathan::clipboard_helper::Dispatcher dispatcher(&sta, &server);
    g_dispatcher.store(&dispatcher);
    sta.SetOnClipboardChanged([&dispatcher]() { dispatcher.OnClipboardChanged(); });
    sta.SetOnRenderFormat([&dispatcher](unsigned int fmt, bool cache_only) {
        dispatcher.OnRenderFormat(fmt, cache_only);
    });

    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    // Build the watchdog. If a parent PID was provided and we can open it,
    // spawn a joinable thread that waits on either the parent's exit or a
    // cancellation event so we can join cleanly even when the parent is still
    // alive at shutdown.
    Watchdog wd;
    wd.parent_pid    = ParseParentPid(argc, argv);
    wd.cancel_event  = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    wd.server        = &server;

    // Prefer an inherited handle to the parent (works across the SYSTEM→user
    // session boundary); fall back to OpenProcess by PID for same-token
    // launches where we can open the parent ourselves.
    if (HANDLE inherited = ParseParentHandle(argc, argv); inherited != nullptr) {
        wd.parent_handle = inherited;
    } else if (wd.parent_pid == 0) {
        LH_LOG_WARN("No --parent-pid/--parent-handle provided; watchdog disabled");
    } else {
        wd.parent_handle = ::OpenProcess(SYNCHRONIZE, FALSE, wd.parent_pid);
        if (wd.parent_handle == nullptr) {
            char err[128];
            std::snprintf(err, sizeof(err),
                          "OpenProcess(parent=%lu) failed: lastError=%lu — watchdog disabled",
                          wd.parent_pid, ::GetLastError());
            LH_LOG_ERROR(err);
        }
    }

    std::thread watchdog;
    if (wd.parent_handle && wd.cancel_event) {
        watchdog = std::thread(RunParentWatchdog, &wd);
    }

    server.Run();

    // Tear-down order: stop the server first (no-op if Run() already returned),
    // then signal the watchdog to wake up and join it, then close the OS
    // handles, then clear g_server so the console handler stops seeing us.
    server.Stop();
    if (wd.cancel_event) {
        ::SetEvent(wd.cancel_event);
    }
    if (watchdog.joinable()) {
        watchdog.join();
    }
    if (wd.parent_handle) {
        ::CloseHandle(wd.parent_handle);
        wd.parent_handle = nullptr;
    }
    if (wd.cancel_event) {
        ::CloseHandle(wd.cancel_event);
        wd.cancel_event = nullptr;
    }

    // Order matters: clear g_dispatcher BEFORE shutting StaWorker down so
    // any late inbound frame the pipe handler is still draining cannot
    // dereference a dead dispatcher. Then drop the STA-bound COM proxy
    // (virtual file IDataObject) by dispatching the Release onto the STA
    // thread while the apartment is still alive — releasing it after
    // OleUninitialize would be undefined behavior.
    g_dispatcher.store(nullptr);
    dispatcher.ReleaseStaBoundResources(&sta);
    sta.Stop();

    g_server.store(nullptr);
    // Drop the console handler so a late Ctrl-C signal doesn't reach into a
    // server object whose destructor is about to run.
    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, FALSE);

    LH_LOG_INFO("leviathan-clipboard-helper exited cleanly");
    return 0;
}
