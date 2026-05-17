#pragma once
//
// parent_watchdog — exit the helper when the parent process dies.
//
// Two complementary mechanisms run in parallel; either firing triggers
// the shutdown callback exactly once:
//
//   1. prctl(PR_SET_PDEATHSIG, SIGTERM)
//      Kernel-delivered SIGTERM the instant our actual parent process
//      dies. Zero polling cost when the parent is healthy.
//
//   2. /proc poll via kill(pid, 0)
//      Belt + braces for the cases prctl misses:
//        - the parent re-parented us (e.g. parent's thread leader
//          exited while another thread continued) — PDEATHSIG already
//          fired for the wrong moment, leaving us orphaned to init
//          but with no shutdown.
//        - --parent-pid points to a DIFFERENT process than getppid()
//          (an unusual but supportable configuration).
//      Polls every kPollInterval; calls shutdown_cb on ESRCH.
//
// The shutdown callback runs on the watchdog thread. Implementations
// typically post a metacall to the Qt event loop (e.g. via
// QMetaObject::invokeMethod) so QCoreApplication::quit fires on the
// main thread.

#include <atomic>
#include <chrono>
#include <functional>
#include <sys/types.h>
#include <thread>

namespace leviathan::clipboard_helper {

class ParentWatchdog {
public:
    // How often the poll fallback wakes up to check the parent PID.
    // 2 seconds keeps shutdown responsive without measurable CPU cost
    // (one kill(pid, 0) syscall per interval).
    static constexpr std::chrono::seconds kPollInterval{2};

    ParentWatchdog();
    ~ParentWatchdog();

    ParentWatchdog(const ParentWatchdog&)            = delete;
    ParentWatchdog& operator=(const ParentWatchdog&) = delete;

    // Install the prctl PDEATHSIG and start the poll thread. `parent_pid`
    // is the PID the helper was told to watch (from --parent-pid);
    // typically equal to getppid() at the time Start() is called but
    // not required to be. shutdown_cb runs on the watchdog thread when
    // the parent is observed to be gone.
    //
    // Re-checks parent identity immediately after installing PDEATHSIG
    // to close the race where parent exited between fork() and Start().
    void Start(::pid_t parent_pid, std::function<void()> shutdown_cb);

    // Stop the poll thread. Idempotent. Called by destructor.
    void Stop();

private:
    void PollLoop();

    ::pid_t                parent_pid_{0};
    std::function<void()>  shutdown_cb_;
    std::thread            worker_;
    std::atomic<bool>      stop_{false};
    std::atomic<bool>      fired_{false};
};

}  // namespace leviathan::clipboard_helper
