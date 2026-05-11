#include "sta_worker.h"

#include <ole2.h>

#include <cstdio>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

constexpr wchar_t kWindowClass[] = L"LeviathanClipboardHelperSTA";

// Custom message: drain pending work-queue entries. Posted by PostTask().
constexpr UINT WM_LEVIATHAN_WORK = WM_USER + 1;

// thread_local pointer so the static WndProcThunk can route messages to the
// owning StaWorker instance without going through a global map.
thread_local StaWorker* tls_self = nullptr;

}  // namespace

StaWorker::StaWorker() = default;

StaWorker::~StaWorker() {
    Stop();
}

void StaWorker::SetOnClipboardChanged(OnClipboardChanged cb) {
    on_clipboard_changed_ = std::move(cb);
}

bool StaWorker::Start() {
    if (thread_.joinable()) {
        return true;  // already running
    }
    thread_ = std::thread([this]() { ThreadMain(); });

    std::unique_lock<std::mutex> lock(init_mu_);
    init_cv_.wait(lock, [this] { return init_done_; });
    return init_ok_;
}

void StaWorker::Stop() {
    if (!thread_.joinable()) {
        return;
    }
    stopping_.store(true);
    const DWORD tid = thread_id_.load();
    if (tid != 0) {
        // Posting WM_QUIT to the thread is the canonical way to unblock its
        // GetMessage loop. PostThreadMessage's WM_QUIT will arrive with a
        // null hwnd and GetMessage returns 0, terminating the loop.
        ::PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    thread_.join();
    thread_id_.store(0);
    hwnd_atom_.store(nullptr);
    hwnd_ = nullptr;
}

void StaWorker::PostTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.push_back(std::move(task));
    }
    // PostMessage to the window: the message pump picks it up and drains
    // the queue on the STA thread. PostThreadMessage to thread_id_ would
    // also work but window-targeted dispatch keeps everything inside the
    // single WndProc and lets the work integrate naturally with other
    // window messages (WM_CLIPBOARDUPDATE, WM_RENDERFORMAT, etc).
    HWND hwnd = hwnd_atom_.load();
    if (hwnd != nullptr) {
        ::PostMessageW(hwnd, WM_LEVIATHAN_WORK, 0, 0);
    }
}

void StaWorker::DrainWorkQueue() {
    std::deque<std::function<void()>> snapshot;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        snapshot.swap(queue_);
    }
    for (auto& fn : snapshot) {
        try {
            fn();
        } catch (...) {
            LH_LOG_ERROR("Exception in STA work item (swallowed)");
        }
    }
}

LRESULT CALLBACK StaWorker::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StaWorker* self = tls_self;
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT StaWorker::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LEVIATHAN_WORK:
            DrainWorkQueue();
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (on_clipboard_changed_) {
                try {
                    on_clipboard_changed_();
                } catch (...) {
                    LH_LOG_ERROR("Exception in OnClipboardChanged (swallowed)");
                }
            }
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void StaWorker::ThreadMain() {
    tls_self = this;
    thread_id_.store(::GetCurrentThreadId());

    auto signal_init = [this](bool ok) {
        std::lock_guard<std::mutex> lock(init_mu_);
        init_ok_   = ok;
        init_done_ = true;
        init_cv_.notify_all();
    };

    // Single-Threaded Apartment. OleInitialize calls CoInitializeEx(STA) plus
    // sets up OLE clipboard and drag-drop. If Go's runtime has already put
    // this thread into MTA (rare for a thread we just created, but the
    // safety dance is cheap), we'd see RPC_E_CHANGED_MODE and would have to
    // walk it back. The C++ helper owns this thread end-to-end so the simple
    // case is enough.
    HRESULT hr = ::OleInitialize(nullptr);
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "OleInitialize failed: hr=0x%08lx", hr);
        LH_LOG_ERROR(buf);
        signal_init(false);
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = &StaWorker::WndProcThunk;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    if (::RegisterClassW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        // ERROR_CLASS_ALREADY_EXISTS (1410) means a prior StaWorker
        // (e.g. after a fast restart cycle) registered the class. Reusing
        // it is fine.
        if (err != 1410) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "RegisterClassW failed: lastError=%lu", err);
            LH_LOG_ERROR(buf);
            ::OleUninitialize();
            signal_init(false);
            return;
        }
    }

    HWND hwnd = ::CreateWindowExW(
        0, kWindowClass, L"leviathan-clipboard-helper",
        0,                              // style
        0, 0, 0, 0,                     // x, y, w, h (message-only window ignores)
        HWND_MESSAGE, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "CreateWindowExW failed: lastError=%lu", ::GetLastError());
        LH_LOG_ERROR(buf);
        ::OleUninitialize();
        signal_init(false);
        return;
    }
    hwnd_ = hwnd;
    hwnd_atom_.store(hwnd);

    // Subscribe to clipboard change notifications. The notifications arrive
    // as WM_CLIPBOARDUPDATE on our hidden window.
    if (!::AddClipboardFormatListener(hwnd)) {
        LH_LOG_WARN("AddClipboardFormatListener failed; CLIPBOARD_CHANGED notifications disabled");
    }

    LH_LOG_INFO("StaWorker initialized (STA + window + format listener ready)");
    signal_init(true);

    PumpMessages();

    // Tear down.
    ::RemoveClipboardFormatListener(hwnd);
    ::DestroyWindow(hwnd);
    hwnd_       = nullptr;
    hwnd_atom_.store(nullptr);
    ::OleUninitialize();
    LH_LOG_INFO("StaWorker exited");
}

void StaWorker::PumpMessages() {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

}  // namespace leviathan::clipboard_helper
