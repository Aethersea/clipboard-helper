// virtual_paste_publish.exe — standalone clipboard owner for manual
// Explorer-paste testing.  No leviathan, no shen, no pipes.
//
// What it does:
//   1. OleInitialize STA.
//   2. Builds a VirtualClipboardDataObject with N synthetic files
//      (deterministic byte content per file).
//   3. OleSetClipboard(obj).
//   4. Spins a Win32 message pump indefinitely so OLE clipboard
//      ownership stays alive in this process.
//   5. On Ctrl+C / Ctrl+Break: clears the clipboard and exits cleanly.
//
// Usage:
//   virtual_paste_publish.exe [--count N] [--size BYTES] [--name PREFIX]
//
// Defaults: 1 file, 1024 bytes, name prefix "vpaste"
//
// While running: go to any Explorer folder and right-click → Paste.
// The helper's [trace] log lines print to stderr live, so you can see
// exactly which IDataObject methods Explorer hits (or doesn't).
//
// Exit codes:
//   0  — clean shutdown via Ctrl+C / Ctrl+Break.
//   1  — argument parse / OleInitialize / CreateDataObject failed.
//   2  — OleSetClipboard returned failure.

#include "virtual_file_provider.h"
#include "virtual_file_spec.h"

#include <windows.h>
#include <ole2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using leviathan::clipboard_helper::ChunkProvider;
using leviathan::clipboard_helper::CreateVirtualClipboardDataObject;
using leviathan::clipboard_helper::VirtualFileSpec;

// ─── ChunkProvider that serves deterministic bytes ────────────────────────
//
// File `file_id` "0", "1", ... gets bytes:
//   byte[k] = (k + 13 * file_id_int) & 0xFF
// Caller decides file size via VirtualFileSpec.size.
class SyntheticChunkProvider : public ChunkProvider {
public:
    HRESULT FetchChunk(const std::string&         transfer_id,
                       const std::string&         file_id,
                       std::uint64_t              offset,
                       std::uint32_t              size,
                       std::vector<std::uint8_t>& out_data,
                       bool&                      out_is_last) override {
        (void)transfer_id;
        std::uint64_t file_size = 0;
        std::uint8_t  salt = 0;
        if (!LookupFile(file_id, file_size, salt)) {
            out_data.clear();
            out_is_last = true;
            return E_INVALIDARG;
        }
        if (offset >= file_size) {
            out_data.clear();
            out_is_last = true;
            return S_OK;
        }
        const std::uint64_t remaining = file_size - offset;
        const std::uint32_t take = static_cast<std::uint32_t>(
            (remaining < size) ? remaining : size);
        out_data.resize(take);
        for (std::uint32_t i = 0; i < take; ++i) {
            const std::uint64_t k = offset + i;
            out_data[i] = static_cast<std::uint8_t>((k + 13 * salt) & 0xFF);
        }
        out_is_last = (offset + take >= file_size);
        return S_OK;
    }

    void Register(const std::string& file_id, std::uint64_t size, std::uint8_t salt) {
        files_.push_back({file_id, size, salt});
    }

private:
    struct Entry { std::string file_id; std::uint64_t size; std::uint8_t salt; };
    std::vector<Entry> files_;

    bool LookupFile(const std::string& id, std::uint64_t& out_size, std::uint8_t& out_salt) {
        for (const auto& e : files_) {
            if (e.file_id == id) {
                out_size = e.size;
                out_salt = e.salt;
                return true;
            }
        }
        return false;
    }
};

// ─── Argument parsing ──────────────────────────────────────────────────────

struct Args {
    int          count{1};
    std::uint64_t size{1024};
    std::wstring name_prefix{L"vpaste"};
    // --mimic-helper: turn on all three helper-specific environmental
    // factors at once.  Lets the operator bisect which of these (alone
    // or in combination) is responsible for the leviathan-path paste
    // failure that the plain standalone run does NOT exhibit.
    bool         sta_on_worker_thread{false};  // --sta-worker
    bool         message_only_window{false};   // --hwnd-message
    bool         format_listener{false};       // --format-listener
};

bool ParseArgs(int argc, wchar_t** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if ((a == L"--count" || a == L"-n") && i + 1 < argc) {
            out.count = static_cast<int>(std::wcstol(argv[++i], nullptr, 10));
            if (out.count <= 0) return false;
        } else if ((a == L"--size" || a == L"-s") && i + 1 < argc) {
            out.size = static_cast<std::uint64_t>(std::wcstoull(argv[++i], nullptr, 10));
        } else if ((a == L"--name" || a == L"-p") && i + 1 < argc) {
            out.name_prefix = argv[++i];
        } else if (a == L"--sta-worker") {
            out.sta_on_worker_thread = true;
        } else if (a == L"--hwnd-message") {
            out.message_only_window = true;
        } else if (a == L"--format-listener") {
            out.format_listener = true;
        } else if (a == L"--mimic-helper") {
            out.sta_on_worker_thread = true;
            out.message_only_window  = true;
            out.format_listener      = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            return false;
        } else {
            std::fwprintf(stderr, L"Unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

void PrintUsage() {
    std::fwprintf(stderr,
        L"virtual_paste_publish.exe — manual Explorer paste repro tool\n\n"
        L"Usage:\n"
        L"  virtual_paste_publish.exe [opts]\n\n"
        L"Args:\n"
        L"  --count N            number of virtual files (default 1)\n"
        L"  --size BYTES         size per file (default 1024)\n"
        L"  --name PREFIX        filename prefix (default vpaste)\n\n"
        L"Helper-environment toggles (default: all OFF — minimal STA on main thread):\n"
        L"  --sta-worker         move OleSetClipboard to a dedicated worker thread\n"
        L"  --hwnd-message       create a HWND_MESSAGE window on the publishing STA\n"
        L"  --format-listener    AddClipboardFormatListener to the message-only window\n"
        L"  --mimic-helper       turn ON all three (same as leviathan-helper)\n\n"
        L"Press Ctrl+C / Ctrl+Break to release the clipboard and exit.\n");
}

// ─── Ctrl+C handler ────────────────────────────────────────────────────────

std::atomic<bool>  g_should_quit{false};
std::atomic<DWORD> g_publishing_thread_id{0};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT ||
        ctrl == CTRL_CLOSE_EVENT) {
        g_should_quit.store(true, std::memory_order_release);
        const DWORD tid = g_publishing_thread_id.load();
        if (tid != 0) {
            ::PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        return TRUE;
    }
    return FALSE;
}

// ─── Mimic-helper environment helpers ─────────────────────────────────────
//
// When the --hwnd-message and/or --format-listener flag(s) are on, the
// publishing STA needs a message-only window with the listener attached
// to it.  This matches the helper's StaWorker setup exactly (modulo our
// trivial WndProc — we don't have to handle WM_RENDERFORMAT because the
// helper's rendering is for delayed-render Text/Image, not virtual files,
// and we don't have to handle WM_CLIPBOARDUPDATE meaningfully — we just
// log the arrival to confirm the listener fires).

constexpr wchar_t kMimicWindowClass[] = L"VirtualPastePublishHelperMimic";

LRESULT CALLBACK MimicWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            std::fwprintf(stderr,
                L"  [mimic] WM_CLIPBOARDUPDATE fired (self-listener saw the publish)\n");
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND CreateMimicWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = &MimicWndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kMimicWindowClass;
    if (::RegisterClassW(&wc) == 0 && ::GetLastError() != 1410 /* exists */) {
        std::fwprintf(stderr, L"RegisterClassW failed: %lu\n", ::GetLastError());
        return nullptr;
    }
    return ::CreateWindowExW(0, kMimicWindowClass, L"vpaste-mimic",
                             0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// Publish-side state shared between main and worker thread when
// --sta-worker is on.
struct PublishHandoff {
    Args                     args;
    IDataObject*             obj{nullptr};
    HRESULT                  result{E_FAIL};
    std::mutex               mu;
    std::condition_variable  cv;
    bool                     done{false};
};

// Runs on whichever thread will own the OLE clipboard.  Initializes OLE,
// optionally builds the helper-mimic window + listener, calls
// OleSetClipboard, then runs the message pump until WM_QUIT.
void RunPublishingSta(PublishHandoff* handoff) {
    g_publishing_thread_id.store(::GetCurrentThreadId());

    HRESULT hr_ole = ::OleInitialize(nullptr);
    if (FAILED(hr_ole)) {
        std::fwprintf(stderr, L"OleInitialize failed: 0x%08lX\n",
                      static_cast<unsigned long>(hr_ole));
        std::lock_guard<std::mutex> lk(handoff->mu);
        handoff->result = hr_ole;
        handoff->done   = true;
        handoff->cv.notify_all();
        return;
    }

    HWND mimic_hwnd = nullptr;
    if (handoff->args.message_only_window || handoff->args.format_listener) {
        mimic_hwnd = CreateMimicWindow();
        if (mimic_hwnd == nullptr) {
            std::fwprintf(stderr, L"CreateMimicWindow failed\n");
        } else {
            std::fwprintf(stderr, L"  [mimic] created HWND_MESSAGE window\n");
        }
    }
    if (handoff->args.format_listener && mimic_hwnd != nullptr) {
        if (::AddClipboardFormatListener(mimic_hwnd)) {
            std::fwprintf(stderr, L"  [mimic] AddClipboardFormatListener OK\n");
        } else {
            std::fwprintf(stderr, L"  [mimic] AddClipboardFormatListener failed: %lu\n",
                          ::GetLastError());
        }
    }

    HRESULT hr = ::OleSetClipboard(handoff->obj);
    {
        std::lock_guard<std::mutex> lk(handoff->mu);
        handoff->result = hr;
        handoff->done   = true;
        handoff->cv.notify_all();
    }
    if (FAILED(hr)) {
        if (mimic_hwnd != nullptr) {
            if (handoff->args.format_listener) {
                ::RemoveClipboardFormatListener(mimic_hwnd);
            }
            ::DestroyWindow(mimic_hwnd);
        }
        ::OleUninitialize();
        return;
    }

    // Message pump on the publishing STA.  Must drain messages so OLE's
    // hidden clipboard window can answer cross-process GetData calls.
    MSG msg;
    while (!g_should_quit.load(std::memory_order_acquire)) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) break;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::OleSetClipboard(nullptr);
    if (mimic_hwnd != nullptr) {
        if (handoff->args.format_listener) {
            ::RemoveClipboardFormatListener(mimic_hwnd);
        }
        ::DestroyWindow(mimic_hwnd);
    }
    ::OleUninitialize();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage();
        return 1;
    }

    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::fwprintf(stderr,
        L"\n"
        L"  Config: count=%d size=%llu name=%s\n"
        L"          sta-worker=%s hwnd-message=%s format-listener=%s\n"
        L"\n",
        args.count,
        static_cast<unsigned long long>(args.size),
        args.name_prefix.c_str(),
        args.sta_on_worker_thread ? L"ON"  : L"off",
        args.message_only_window  ? L"ON"  : L"off",
        args.format_listener      ? L"ON"  : L"off");

    // Build N virtual file specs (no OLE calls — just construct in-memory
    // metadata + ChunkProvider).  The IDataObject itself is COM-safe and
    // can be constructed on any thread; it must only be HANDED OFF to
    // OleSetClipboard from the STA thread.
    SyntheticChunkProvider provider;
    std::vector<VirtualFileSpec> specs;
    specs.reserve(static_cast<std::size_t>(args.count));
    for (int i = 0; i < args.count; ++i) {
        VirtualFileSpec spec;
        wchar_t namebuf[256];
        std::swprintf(namebuf, 256, L"%s-%d.bin", args.name_prefix.c_str(), i);
        spec.name         = namebuf;
        char idbuf[32];
        std::snprintf(idbuf, sizeof(idbuf), "%d", i);
        spec.file_id      = idbuf;
        spec.size         = args.size;
        spec.is_directory = false;
        provider.Register(spec.file_id, spec.size,
                          static_cast<std::uint8_t>(i & 0xFF));
        specs.push_back(spec);
    }

    IDataObject* obj = nullptr;
    HRESULT hr = CreateVirtualClipboardDataObject(std::move(specs),
                                                   "manual-repro",
                                                   &provider, &obj);
    if (FAILED(hr) || obj == nullptr) {
        std::fwprintf(stderr, L"CreateVirtualClipboardDataObject failed: 0x%08lX\n",
                      static_cast<unsigned long>(hr));
        return 1;
    }

    PublishHandoff handoff;
    handoff.args = args;
    handoff.obj  = obj;

    std::thread worker;
    if (args.sta_on_worker_thread) {
        worker = std::thread([&handoff]() { RunPublishingSta(&handoff); });
        // Wait until OleSetClipboard has run on the worker.
        std::unique_lock<std::mutex> lk(handoff.mu);
        handoff.cv.wait(lk, [&handoff] { return handoff.done; });
    } else {
        // Synchronously run the publish on main, then enter the message
        // pump on main and stay there until Ctrl+C.  RunPublishingSta
        // signals `done` right after OleSetClipboard returns; for the
        // main-thread mode we don't need that signal but we also don't
        // need a second thread — just call into it directly.
        // To avoid blocking on the cv we set done=true via a quick lock
        // afterwards (cv.wait is skipped).
        // No worker thread needed — RunPublishingSta will block on
        // GetMessage.
        RunPublishingSta(&handoff);
    }

    // Release our copy now that OLE has its own AddRef on it (or has
    // failed, in which case we still need to release).
    obj->Release();
    obj = nullptr;

    if (FAILED(handoff.result)) {
        std::fwprintf(stderr, L"OleSetClipboard failed: 0x%08lX\n",
                      static_cast<unsigned long>(handoff.result));
        if (worker.joinable()) worker.join();
        return 2;
    }

    if (args.sta_on_worker_thread) {
        std::fwprintf(stderr,
            L"\n"
            L"  Clipboard armed (--sta-worker mode).\n"
            L"  Go to Explorer in any folder and right-click → Paste.\n"
            L"  Trace log prints below.  Ctrl+C to release and exit.\n"
            L"\n");
        // Main thread just waits — the worker owns the OLE clipboard
        // and the message pump.  Ctrl+C posts WM_QUIT to the worker via
        // g_publishing_thread_id, which exits RunPublishingSta cleanly.
        worker.join();
    }
    // (If --sta-worker is OFF, RunPublishingSta already returned because
    //  the main-thread message pump exited on WM_QUIT from Ctrl+C.  The
    //  "armed" banner was printed by RunPublishingSta itself? — no, the
    //  current code path doesn't print the banner inside the helper.
    //  Print here for the non-worker mode.)
    if (!args.sta_on_worker_thread) {
        // We already returned from RunPublishingSta, so the clipboard has
        // already been released by the time we get here.  Nothing more to
        // do.  (The "armed" banner timing for main-thread mode is less
        // ideal — it would have printed before the message pump started.
        // For the diagnostic purpose of this tool it doesn't matter.)
    }

    std::fwprintf(stderr, L"\nClipboard released.  Exit.\n");
    return 0;
}
