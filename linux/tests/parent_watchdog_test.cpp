// parent_watchdog unit tests — exercise the prctl + /proc poll
// mechanisms with real child processes.
//
// Two patterns:
//   1. Watch a known-dead PID → shutdown_cb fires synchronously
//      inside Start() (the "parent already gone at watchdog start"
//      race-closing check).
//   2. Watch a real child PID, kill it, expect the poll thread to
//      observe ESRCH within ~3 poll intervals.
//
// We avoid asserting on PDEATHSIG itself — it only fires if the test
// runner happens to be the *thread leader* of the child's parent
// process, which is fragile under threaded test runners. The /proc
// poll fallback is what the tests pin down.

#include "parent_watchdog.h"
#include "test_lite.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

using leviathan::clipboard_helper::ParentWatchdog;

namespace {

// Fork a child that blocks on pause() until signalled, returns its PID.
// On fork failure returns -1 (test should ASSERT).
::pid_t SpawnPausedChild() {
    const ::pid_t pid = ::fork();
    if (pid == 0) {
        // Child: block until signal. _exit avoids running atexit handlers
        // which could touch test framework globals.
        ::pause();
        ::_exit(0);
    }
    return pid;
}

// Reap a child cleanly. Used by test teardown — best-effort.
void ReapChild(::pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

// Poll for `pred()` becoming true, up to `timeout` total. Returns true
// if pred fires, false on timeout. Polls in 50 ms slices to keep
// tests responsive.
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

}  // namespace

// ─── Synchronous "already dead at Start" path ────────────────────────────

TEST(ParentWatchdog, AlreadyDeadParentFiresShutdownSynchronously) {
    // Fork a child, kill it, reap it, THEN start a watchdog on its
    // (now-stale) PID. PID reuse is racy but vanishingly unlikely
    // within microseconds on Linux, where the kernel allocates PIDs
    // sequentially up to /proc/sys/kernel/pid_max. The test treats
    // the synchronous-fire path as the contract: if the parent is
    // gone at the moment Start() runs, the callback must fire before
    // Start() returns.
    const ::pid_t child = SpawnPausedChild();
    ASSERT_TRUE(child > 0);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    std::atomic<int> fired{0};
    ParentWatchdog w;
    w.Start(child, [&fired] { fired.fetch_add(1); });

    // Synchronous semantics: the early-exit path inside Start() calls
    // the callback before returning. No need to wait.
    EXPECT_EQ(fired.load(), 1);
    w.Stop();
}

// ─── Asynchronous poll path ──────────────────────────────────────────────

TEST(ParentWatchdog, KilledParentFiresShutdownViaPollLoop) {
    const ::pid_t child = SpawnPausedChild();
    ASSERT_TRUE(child > 0);

    std::atomic<int> fired{0};
    ParentWatchdog w;
    w.Start(child, [&fired] { fired.fetch_add(1); });

    // Child is alive — give the watchdog enough time to run a poll
    // cycle and observe ESRCH after the kill. kPollInterval is 2s,
    // plus we account for the test's 200 ms slice quantum, plus a
    // generous margin for CI noise → 6 s.
    EXPECT_EQ(fired.load(), 0);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    const bool got_fire = WaitFor(
        [&fired] { return fired.load() == 1; },
        std::chrono::seconds(6));
    EXPECT_TRUE(got_fire);
    w.Stop();
}

TEST(ParentWatchdog, LivingParentDoesNotFire) {
    // Watch our OWN pid — guaranteed alive throughout the test.
    // Verify the callback never fires during a >1 poll interval window.
    std::atomic<int> fired{0};
    ParentWatchdog w;
    w.Start(::getpid(), [&fired] { fired.fetch_add(1); });

    // kPollInterval is 2 s; sleep 3 s so the poll thread definitely
    // ran ≥1 check.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_EQ(fired.load(), 0);
    w.Stop();
}

// ─── Callback is fired at most once ──────────────────────────────────────

TEST(ParentWatchdog, ShutdownFiresAtMostOnceEvenAcrossPollAndSync) {
    // Synchronous path: parent already dead at Start time. Even if
    // Start somehow also spawned the poll thread, the `fired_` atomic
    // guards against double-fire.
    const ::pid_t child = SpawnPausedChild();
    ASSERT_TRUE(child > 0);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    std::atomic<int> fired{0};
    ParentWatchdog w;
    w.Start(child, [&fired] { fired.fetch_add(1); });
    // Give any latent poll thread a chance to also fire (it shouldn't,
    // but if the implementation regresses we'll catch it here).
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(fired.load(), 1);
    w.Stop();
}

// ─── Stop is idempotent ──────────────────────────────────────────────────

TEST(ParentWatchdog, StopIsIdempotent) {
    ParentWatchdog w;
    w.Start(::getpid(), [] {});
    w.Stop();
    w.Stop();  // must not crash or hang
}

TEST(ParentWatchdog, StopWithoutStartIsSafe) {
    ParentWatchdog w;
    w.Stop();  // never started → no thread to join, must no-op
}

// ─── Destructor stops cleanly ────────────────────────────────────────────

TEST(ParentWatchdog, DestructorStopsPollThread) {
    // If the destructor didn't join the thread, the test would leak
    // a worker that outlives the ParentWatchdog. There's no clean
    // observable signal for that, but creating + destroying many
    // watchdogs without resource exhaustion is a smoke check.
    for (int i = 0; i < 10; ++i) {
        ParentWatchdog w;
        w.Start(::getpid(), [] {});
        // Destructor on scope exit must Stop() cleanly.
    }
    // If we got here without hangs or fd-exhaustion, the destructor
    // is doing its job.
    EXPECT_TRUE(true);
}
