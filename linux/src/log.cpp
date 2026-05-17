#include "log.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace leviathan::clipboard_helper {

namespace {

std::mutex g_mutex;
std::atomic<bool> g_verbose{false};

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void SetVerbose(bool enabled) {
    g_verbose.store(enabled, std::memory_order_relaxed);
}

bool VerboseEnabled() {
    return g_verbose.load(std::memory_order_relaxed);
}

std::size_t FormatLogLine(LogLevel                              level,
                          std::chrono::system_clock::time_point now,
                          std::uint32_t                         pid,
                          std::string_view                      msg,
                          char*                                 out,
                          std::size_t                           out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(now);
    // duration_cast on a signed rep returns a signed count; before-epoch
    // times would yield a negative remainder under C++'s truncated-division
    // modulo. Re-fold into [0, 999] so the %03lld field never prints "-1".
    const auto raw_ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto ms     = (raw_ms + 1000) % 1000;

    std::tm tm_buf{};
    // localtime_r is the POSIX thread-safe variant; returns nullptr on
    // out-of-range inputs (e.g. a sentinel time_t::max() that test fixtures
    // might pass).
    if (::localtime_r(&t, &tm_buf) == nullptr) {
        return 0;
    }

    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<long long>(ms));

    // %.*s precision is `int`; clamp the string_view length so a >2 GiB
    // input doesn't overflow. Past INT_MAX the line is already too long to
    // log meaningfully anyway.
    const int precision = (msg.size() > static_cast<std::size_t>(INT_MAX))
                              ? INT_MAX
                              : static_cast<int>(msg.size());
    const int written_raw = std::snprintf(out, out_size,
                                          "[%s] [%s] [helper:%lu] %.*s\n",
                                          timestamp,
                                          LevelTag(level),
                                          static_cast<unsigned long>(pid),
                                          precision,
                                          msg.data());
    if (written_raw <= 0) {
        return 0;
    }
    const std::size_t written_unclamped = static_cast<std::size_t>(written_raw);
    return std::min<std::size_t>(written_unclamped, out_size - 1);
}

void LogMessage(LogLevel level, std::string_view msg) {
    if (level == LogLevel::Debug && !VerboseEnabled()) {
        return;
    }

    char line[2048];
    const std::size_t written = FormatLogLine(level,
                                              std::chrono::system_clock::now(),
                                              static_cast<std::uint32_t>(::getpid()),
                                              msg,
                                              line,
                                              sizeof(line));
    if (written == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    // Write to stderr unbuffered. fwrite returns count of items written,
    // not bytes — we use 1-byte items so the return == bytes.
    std::fwrite(line, 1, written, stderr);
    std::fflush(stderr);
}

}  // namespace leviathan::clipboard_helper
