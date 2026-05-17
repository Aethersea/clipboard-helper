#include "work_queue.h"

#include <utility>

namespace leviathan::clipboard_helper {

void WorkQueue::Push(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    queue_.push_back(std::move(task));
}

WorkQueue::DrainStats WorkQueue::Drain() {
    std::deque<std::function<void()>> snapshot;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        snapshot.swap(queue_);
    }
    DrainStats stats;
    for (auto& fn : snapshot) {
        ++stats.executed;
        try {
            fn();
        } catch (...) {
            ++stats.exceptions;
        }
    }
    return stats;
}

void WorkQueue::DropAll() {
    std::deque<std::function<void()>> leftover;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        leftover.swap(queue_);
    }
    // `leftover` goes out of scope here; destructors run, which in
    // turn destroy any captured std::packaged_task and surface
    // std::future_error(broken_promise) on the owning futures.
}

std::size_t WorkQueue::Size() const {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return queue_.size();
}

}  // namespace leviathan::clipboard_helper
