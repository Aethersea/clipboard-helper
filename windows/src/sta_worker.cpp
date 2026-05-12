#include "sta_worker.h"

#include <ole2.h>

#include <cstdio>

#include "clipboard_ops.h"
#include "log.h"
#include "wic_image.h"

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
    std::lock_guard<std::mutex> lock(callback_mu_);
    on_clipboard_changed_ = std::move(cb);
}

void StaWorker::SetOnRenderFormat(OnRenderFormat cb) {
    std::lock_guard<std::mutex> lock(callback_mu_);
    on_render_format_ = std::move(cb);
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
        case WM_CLIPBOARDUPDATE: {
            // Snapshot the callback under the lock so a concurrent setter
            // cannot tear the std::function we are about to invoke.
            OnClipboardChanged cb;
            {
                std::lock_guard<std::mutex> lock(callback_mu_);
                cb = on_clipboard_changed_;
            }
            if (cb) {
                try {
                    cb();
                } catch (...) {
                    LH_LOG_ERROR("Exception in OnClipboardChanged (swallowed)");
                }
            }
            return 0;
        }
        case WM_RENDERFORMAT: {
            // The OS opened the clipboard before posting this message and
            // expects us to call SetClipboardData(format, hglobal) before
            // returning. The handler does ALL the round-tripping (issue
            // DATA_REQUEST, wait for PROVIDE_DATA, materialize). If the
            // handler is unset we leave the format slot empty — paste in
            // the remote app will see "no data" which is the least-broken
            // outcome.
            //
            // WM_LEVIATHAN_WORK is *not* deferred during this handler's
            // inner message pump: deferring would wedge any pipe-thread
            // RunSync for up to kRenderTimeoutMs. Instead we mark the
            // thread as "OS owns the clipboard" via OsClipboardHeldGuard
            // so any nested clipboard-touching lambda (ClipboardScope /
            // OleGetClipboard) bails immediately with a warning rather
            // than burning 150 ms in OpenClipboard retries.
            OnRenderFormat cb;
            {
                std::lock_guard<std::mutex> lock(callback_mu_);
                cb = on_render_format_;
            }
            if (cb) {
                OsClipboardHeldGuard render_guard;
                try {
                    cb(static_cast<unsigned int>(wp), /*cache_only=*/false);
                } catch (...) {
                    LH_LOG_ERROR("Exception in OnRenderFormat (swallowed)");
                }
            }
            return 0;
        }
        case WM_RENDERALLFORMATS: {
            // Fired when the system is about to deliver our advertised
            // formats to another process and we are about to lose ownership.
            // We can NOT afford the 120s-per-format round-trip the regular
            // WM_RENDERFORMAT path budgets — that would stall the whole
            // system clipboard for minutes. Invoke the callback in cache-only
            // mode: each format renders only if the data is already cached
            // by a prior WM_RENDERFORMAT for the same announcement; otherwise
            // the slot is left empty.
            OnRenderFormat cb;
            {
                std::lock_guard<std::mutex> lock(callback_mu_);
                cb = on_render_format_;
            }
            if (cb) {
                const unsigned int fmts[] = { CF_UNICODETEXT, CF_DIBV5, CF_DIB, CF_HDROP };
                if (::OpenClipboard(hwnd)) {
                    OsClipboardHeldGuard render_guard;
                    for (unsigned int fmt : fmts) {
                        if (::IsClipboardFormatAvailable(fmt)) {
                            try { cb(fmt, /*cache_only=*/true); }
                            catch (...) { LH_LOG_ERROR("OnRenderFormat threw in WM_RENDERALLFORMATS"); }
                        }
                    }
                    ::CloseClipboard();
                }
            }
            return 0;
        }
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
    hwnd_atom_.store(hwnd);

    // Subscribe to clipboard change notifications. The notifications arrive
    // as WM_CLIPBOARDUPDATE on our hidden window.
    if (!::AddClipboardFormatListener(hwnd)) {
        LH_LOG_WARN("AddClipboardFormatListener failed; CLIPBOARD_CHANGED notifications disabled");
    }

    LH_LOG_INFO("StaWorker initialized (STA + window + format listener ready)");
    signal_init(true);

    PumpMessages();

    // Tear down. Clear the public atom *before* DestroyWindow so any thread
    // currently in HwndOwner() cannot hold a dangling HWND for long.
    hwnd_atom_.store(nullptr);

    // Drain any pending work items that were queued but never dispatched
    // (e.g. a RunSync racing the WM_QUIT that brought us here). Destroying
    // the captured packaged_tasks without invoking them surfaces as
    // std::future_error(broken_promise) on the caller's future.get(), which
    // is far better than the caller hanging forever.
    {
        std::deque<std::function<void()>> leftover;
        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            leftover.swap(queue_);
        }
        leftover.clear();  // destructors run here, breaking pending promises.
    }

    ::RemoveClipboardFormatListener(hwnd);
    ::DestroyWindow(hwnd);
    // Drop the cached WIC factory while COM is still initialized on this
    // thread; otherwise the thread_local destructor in wic_image.cpp would
    // run after OleUninitialize and release a COM interface against a
    // torn-down apartment (UB).
    ResetWicFactoryForThread();
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
