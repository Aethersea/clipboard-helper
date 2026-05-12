#pragma once
//
// sta_worker — a single-threaded apartment (STA) Win32 thread that owns all
// OLE clipboard interaction. Required because:
//
//   * OleGetClipboard / OleSetClipboard expect an STA-initialized thread that
//     also pumps a message loop (the OLE clipboard implementation marshals
//     IDataObject calls through window messages).
//   * IDataObject server-side, WM_RENDERFORMAT and WM_CLIPBOARDUPDATE all
//     require a window with a message loop on the owning thread.
//
// The worker is constructed on the helper's main thread, started with
// Start(), and shut down with Stop(). Other threads schedule work using
// RunSync (request/reply) or PostAsync (fire-and-forget); both queue a
// callable that the STA thread executes inside its message loop.
//
// The worker also installs an AddClipboardFormatListener subscription and
// invokes the OnClipboardChanged callback on every WM_CLIPBOARDUPDATE.

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace leviathan::clipboard_helper {

// OnClipboardChanged is invoked on the STA thread whenever the OS reports a
// clipboard sequence change. The callback is expected to be quick; defer any
// heavy work (e.g. encoding image to PNG) to a different thread.
using OnClipboardChanged = std::function<void()>;

// OnRenderFormat fires on the STA thread when the OS asks us to materialize
// delayed-render data we previously advertised via SetClipboardData(fmt, NULL).
// The callback runs *inside* the WM_RENDERFORMAT handler — the clipboard is
// already open, and the callback is expected to call SetClipboardData(fmt, ...)
// before returning. The Win32 parameter is the requested clipboard format
// (CF_UNICODETEXT, CF_DIBV5, etc).
using OnRenderFormat = std::function<void(unsigned int /*format*/)>;

class StaWorker {
public:
    StaWorker();
    ~StaWorker();

    StaWorker(const StaWorker&) = delete;
    StaWorker& operator=(const StaWorker&) = delete;

    // Spawns the STA thread and waits for it to finish initializing (OLE +
    // window created + listener installed). Returns false on init failure.
    bool Start();

    // Posts WM_QUIT to the STA thread and joins. Safe to call multiple times.
    void Stop();

    // Registers the change callback. Safe to set before or after Start.
    void SetOnClipboardChanged(OnClipboardChanged cb);

    // Registers the WM_RENDERFORMAT handler. Safe to set before or after
    // Start. The handler runs ON THE STA THREAD inside the OS-driven
    // WM_RENDERFORMAT dispatch; the clipboard is already open and the
    // handler is expected to call SetClipboardData synchronously.
    void SetOnRenderFormat(OnRenderFormat cb);

    // Schedules `fn` to run on the STA thread and returns once it completes.
    // Throwing from `fn` is forwarded to the caller via the future.
    //
    // The packaged_task is wrapped in a shared_ptr because the work queue
    // is std::function<void()>, which requires the captured callable to be
    // copyable. packaged_task itself is move-only; the shared_ptr indirection
    // gives us a copyable handle without dragging in std::move_only_function
    // (C++23) or hand-rolling a move-only erased callable.
    template <typename F>
    auto RunSync(F&& fn) -> decltype(fn()) {
        using Result = decltype(fn());
        auto task   = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(fn));
        auto future = task->get_future();
        PostTask([task]() { (*task)(); });
        return future.get();
    }

    // The HWND of the worker's hidden message-only window. Returns nullptr
    // before Start() completes successfully. Useful for callers that need
    // to advertise an OLE clipboard owner (delayed rendering, drag/drop).
    //
    // Reads via the atomic so callers on threads other than the STA worker
    // see a coherent value during the brief windows where ThreadMain is
    // creating or destroying the window.
    HWND HwndOwner() const { return hwnd_atom_.load(); }

    // The DWORD thread ID of the STA worker, or 0 if not running. Used by
    // callers that want to PostThreadMessage directly.
    DWORD ThreadId() const { return thread_id_.load(); }

private:
    void ThreadMain();
    void PumpMessages();
    void DrainWorkQueue();
    void PostTask(std::function<void()> task);

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    std::thread          thread_;
    std::atomic<DWORD>   thread_id_{0};
    // The published HWND. Visible to other threads via HwndOwner() once
    // ThreadMain has assigned it; ThreadMain clears it before destroying
    // the window so callers do not see a dangling handle.
    std::atomic<HWND>    hwnd_atom_{nullptr};

    // Synchronizes Start completion (caller blocks until ready or failure).
    std::mutex                  init_mu_;
    std::condition_variable     init_cv_;
    bool                        init_done_{false};
    bool                        init_ok_{false};

    // Work queue. Items are popped on the STA thread inside WndProc when
    // the custom WM_LEVIATHAN_WORK message arrives.
    std::mutex                          queue_mu_;
    std::deque<std::function<void()>>   queue_;

    // on_clipboard_changed_ is set by external threads (SetOnClipboardChanged)
    // and read by the STA thread inside WndProc when WM_CLIPBOARDUPDATE
    // fires. The mutex prevents a torn read of the std::function during a
    // concurrent setter, even though the typical lifecycle (set once at
    // boot, never reassigned) makes the contention vanishingly rare.
    std::mutex           callback_mu_;
    OnClipboardChanged   on_clipboard_changed_;
    OnRenderFormat       on_render_format_;
    std::atomic<bool>    stopping_{false};
};

}  // namespace leviathan::clipboard_helper
