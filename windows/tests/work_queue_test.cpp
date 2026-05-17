// Pure-logic tests for WorkQueue — the FIFO of std::function<void()>
// that StaWorker dispatches inside its WM_LEVIATHAN_WORK handler.
// Exercising it here lets us verify the queueing / draining / drop-on-
// shutdown semantics without spinning up an STA thread or a hidden
// message-only window.

#include "test_lite.h"
#include "work_queue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using leviathan::clipboard_helper::WorkQueue;

// ─── FIFO + size ──────────────────────────────────────────────────────────

TEST(WorkQueue, EmptyQueueSize) {
    WorkQueue q;
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(0));
}

TEST(WorkQueue, PushIncrementsSize) {
    WorkQueue q;
    q.Push([]() {});
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(1));
    q.Push([]() {});
    q.Push([]() {});
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(3));
}

TEST(WorkQueue, DrainExecutesInFifoOrder) {
    WorkQueue q;
    std::vector<int> order;
    q.Push([&]() { order.push_back(1); });
    q.Push([&]() { order.push_back(2); });
    q.Push([&]() { order.push_back(3); });

    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(3));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(0));

    ASSERT_EQ(order.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(0));
}

TEST(WorkQueue, DrainOnEmptyReturnsZeroStats) {
    WorkQueue q;
    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(0));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(0));
}

// ─── Exception swallowing ─────────────────────────────────────────────────

TEST(WorkQueue, ExceptionFromOneTaskIsCountedAndOthersStillRun) {
    WorkQueue q;
    std::vector<int> ran;
    q.Push([&]() { ran.push_back(1); });
    q.Push([]() { throw std::runtime_error("boom"); });
    q.Push([&]() { ran.push_back(3); });

    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(3));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(1));

    // Tasks before AND after the throwing one must have run — the
    // swallow contract is per-task, not per-batch.
    ASSERT_EQ(ran.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(ran[0], 1);
    EXPECT_EQ(ran[1], 3);
}

TEST(WorkQueue, NonStandardExceptionAlsoSwallowed) {
    WorkQueue q;
    bool ran_after = false;
    q.Push([]() { throw 42; });           // not derived from std::exception
    q.Push([&]() { ran_after = true; });

    const auto stats = q.Drain();
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(1));
    EXPECT_TRUE(ran_after);
}

// ─── DropAll ──────────────────────────────────────────────────────────────

TEST(WorkQueue, DropAllRemovesQueuedTasksWithoutRunningThem) {
    WorkQueue q;
    bool ran = false;
    q.Push([&]() { ran = true; });
    q.Push([&]() { ran = true; });
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(2));

    q.DropAll();
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(0));
    EXPECT_FALSE(ran);

    // Subsequent Drain is a no-op.
    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(0));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(0));
}

TEST(WorkQueue, DropAllBreaksPendingPackagedTaskPromise) {
    // The shutdown contract: RunSync handed off a packaged_task; if the
    // queue is dropped before drain, the captured task is destroyed,
    // which causes std::future_error(broken_promise) on the caller's
    // future.get() — far better than wedging forever.
    //
    // Move the shared_ptr ownership INTO the lambda so the only live
    // reference dies with the queue's stored callable. Holding an outer
    // shared_ptr in the test fixture would keep the packaged_task alive
    // after DropAll, leaving future.get() blocked on an unfulfilled
    // promise (the original test-author bug).
    WorkQueue q;
    auto owner  = std::make_shared<std::packaged_task<int()>>([]() { return 42; });
    auto future = owner->get_future();
    q.Push([t = std::move(owner)]() { (*t)(); });
    ASSERT_TRUE(owner == nullptr);  // moved into the lambda

    q.DropAll();

    bool got_future_error = false;
    try {
        future.get();
    } catch (const std::future_error&) {
        got_future_error = true;
    }
    EXPECT_TRUE(got_future_error);
}

// ─── packaged_task round-trip (matches RunSync<T>() shape) ────────────────

TEST(WorkQueue, PackagedTaskResultReachesFuture) {
    WorkQueue q;
    auto task   = std::make_shared<std::packaged_task<int()>>([]() { return 7; });
    auto future = task->get_future();
    q.Push([task]() { (*task)(); });

    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(1));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(0));
    EXPECT_EQ(future.get(), 7);
}

TEST(WorkQueue, PackagedTaskExceptionReachesFuture) {
    // packaged_task captures exceptions internally; the WorkQueue's
    // own swallow shouldn't fire because invoke does not re-throw.
    WorkQueue q;
    auto task = std::make_shared<std::packaged_task<int()>>([]() -> int {
        throw std::runtime_error("inner-fail");
    });
    auto future = task->get_future();
    q.Push([task]() { (*task)(); });

    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,   static_cast<std::size_t>(1));
    EXPECT_EQ(stats.exceptions, static_cast<std::size_t>(0));

    bool propagated = false;
    try {
        (void)future.get();
    } catch (const std::runtime_error& e) {
        propagated = (std::string(e.what()) == "inner-fail");
    }
    EXPECT_TRUE(propagated);
}

// ─── Concurrent producers / single consumer ───────────────────────────────

TEST(WorkQueue, ConcurrentProducersThenSingleDrainGetsAllItems) {
    WorkQueue q;
    constexpr int kProducers     = 4;
    constexpr int kItemsPerThread = 250;
    std::atomic<int> executed{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&]() {
            for (int i = 0; i < kItemsPerThread; ++i) {
                q.Push([&]() { executed.fetch_add(1); });
            }
        });
    }
    for (auto& th : producers) th.join();

    EXPECT_EQ(q.Size(),
              static_cast<std::size_t>(kProducers * kItemsPerThread));
    const auto stats = q.Drain();
    EXPECT_EQ(stats.executed,
              static_cast<std::size_t>(kProducers * kItemsPerThread));
    EXPECT_EQ(executed.load(), kProducers * kItemsPerThread);
}

// ─── Re-entrant Push from within a draining task ──────────────────────────

TEST(WorkQueue, TaskRePushingDuringDrainQueuesForNextDrain) {
    // The Drain implementation swaps into a local snapshot under the
    // mutex, then releases the mutex before invoking each task. A
    // task that calls q.Push() during execution must succeed (no
    // deadlock) and the new task must NOT be drained until the next
    // Drain() call.
    WorkQueue q;
    int second_pass_count = 0;
    q.Push([&]() {
        q.Push([&]() { ++second_pass_count; });
    });

    auto stats = q.Drain();
    EXPECT_EQ(stats.executed, static_cast<std::size_t>(1));
    EXPECT_EQ(second_pass_count, 0);  // re-pushed task not yet run
    EXPECT_EQ(q.Size(), static_cast<std::size_t>(1));

    stats = q.Drain();
    EXPECT_EQ(stats.executed, static_cast<std::size_t>(1));
    EXPECT_EQ(second_pass_count, 1);
}
