#pragma once
//
// work_queue — pure FIFO of std::function<void()> work items, guarded
// by a single mutex.
//
// Carved out of StaWorker so the queue plumbing can be unit-tested
// without an STA thread, a message-only window, or Win32 message
// dispatch. The OS-side glue (PostMessage(WM_LEVIATHAN_WORK), the
// hidden window's WndProc that calls Drain on receipt) stays in
// sta_worker.cpp.
//
// Thread-safety: every public method is safe to call from any thread.
// Push is typically called from pipe-server / dispatcher threads;
// Drain is called exclusively from the STA worker thread inside its
// WM_LEVIATHAN_WORK handler.

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace leviathan::clipboard_helper {

class WorkQueue {
public:
    struct DrainStats {
        std::size_t executed{0};    // total invocations attempted
        std::size_t exceptions{0};  // those that threw and were swallowed
    };

    // Append a task to the back of the queue.
    void Push(std::function<void()> task);

    // Move every queued task into a local buffer, then invoke each one
    // in order. Exceptions from individual tasks are swallowed and
    // counted. The mutex is held only during the swap, so tasks
    // re-entering Push from their own bodies do not deadlock.
    DrainStats Drain();

    // Drop every queued task without invoking. Destructors run as the
    // local buffer goes out of scope — for std::packaged_task captures
    // this surfaces as std::future_error(broken_promise) on the
    // caller's future.get(), so a RunSync racing shutdown fails fast
    // rather than wedging forever.
    void DropAll();

    // Best-effort snapshot of the current queue size. Useful for tests
    // and for sta_worker debug telemetry.
    std::size_t Size() const;

private:
    mutable std::mutex                queue_mu_;
    std::deque<std::function<void()>> queue_;
};

}  // namespace leviathan::clipboard_helper
