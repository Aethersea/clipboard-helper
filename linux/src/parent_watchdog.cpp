#include "parent_watchdog.h"

#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <utility>

#include "log.h"

namespace leviathan::clipboard_helper {

ParentWatchdog::ParentWatchdog() = default;

ParentWatchdog::~ParentWatchdog() {
    Stop();
}

void ParentWatchdog::Start(::pid_t parent_pid, std::function<void()> shutdown_cb) {
    parent_pid_  = parent_pid;
    shutdown_cb_ = std::move(shutdown_cb);

    // Install PDEATHSIG. This sets a per-process attribute, so it
    // persists across fork (which we don't do, but worth noting).
    // SIGTERM matches the macOS / Windows graceful-shutdown signal —
    // main.cpp registers a handler that drives QCoreApplication::quit.
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        std::ostringstream m;
        m << "prctl(PR_SET_PDEATHSIG, SIGTERM) failed: " << std::strerror(errno);
        LH_LOG_WARN(m.str());
        // Continue — the poll fallback still works without PDEATHSIG.
    }

    // Close the race: parent may have exited between fork() and Start();
    // PDEATHSIG would never fire for an already-dead parent. Check once
    // synchronously before spawning the poll thread.
    if (::kill(parent_pid_, 0) != 0 && errno == ESRCH) {
        std::ostringstream m;
        m << "Parent PID " << parent_pid_
          << " already gone at watchdog start; firing shutdown immediately";
        LH_LOG_WARN(m.str());
        if (!fired_.exchange(true) && shutdown_cb_) {
            shutdown_cb_();
        }
        return;
    }

    std::ostringstream m;
    m << "ParentWatchdog watching PID " << parent_pid_
      << " (PDEATHSIG=SIGTERM, poll interval " << kPollInterval.count() << "s)";
    LH_LOG_INFO(m.str());

    worker_ = std::thread([this] { PollLoop(); });
}

void ParentWatchdog::Stop() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

void ParentWatchdog::PollLoop() {
    using namespace std::chrono;
    while (!stop_.load(std::memory_order_acquire)) {
        // sleep_for is interruptible by signal but not by a stop_ flip;
        // sleep in small slices so Stop() doesn't hang Drop() up to the
        // full poll interval. 200 ms slices keep CPU cost negligible.
        const auto deadline = steady_clock::now() + kPollInterval;
        while (steady_clock::now() < deadline) {
            if (stop_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(milliseconds(200));
        }

        // kill(pid, 0) probes existence without delivering a signal.
        // EPERM means the process exists but we lack permission — that
        // still counts as "alive" for our purposes (parent is running,
        // it just changed UID/GID since we attached). Only ESRCH means
        // gone.
        if (::kill(parent_pid_, 0) != 0 && errno == ESRCH) {
            std::ostringstream m;
            m << "Parent PID " << parent_pid_
              << " no longer exists; triggering shutdown";
            LH_LOG_INFO(m.str());
            if (!fired_.exchange(true) && shutdown_cb_) {
                shutdown_cb_();
            }
            return;
        }
    }
}

}  // namespace leviathan::clipboard_helper
