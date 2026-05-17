#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace leviathan::clipboard_helper {

namespace {

std::mutex g_mutex;
std::wstring g_process_name;

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
    const auto t  = system_clock::to_time_t(now);
    // duration_cast on a signed rep returns a signed count; before-epoch
    // times would yield a negative remainder under C++'s truncated-division
    // modulo. Re-fold into [0, 999] so the %03lld field never prints "-1".
    const auto raw_ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto ms     = (raw_ms + 1000) % 1000;

    std::tm tm_buf{};
    // localtime_s on the CRT goes through TZ tables; cap the time_t at
    // the moderate-future range gmtime_s also accepts so a sentinel
    // value (e.g. time_t::max()) in tests doesn't return a null tm.
    if (localtime_s(&tm_buf, &t) != 0) {
        return 0;
    }

    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<long long>(ms));

    // snprintf returns the number of characters that WOULD have been written
    // if the buffer were unlimited (excluding the NUL). Clamp to out_size-1
    // before reporting bytes-written, otherwise callers that pass the value
    // to WriteFile would read past `out`.
    //
    // The %.*s precision is taken as `int`; clamping msg.size() to INT_MAX
    // avoids signed overflow on the unusual case of a string_view larger
    // than 2 GiB. Anything past this gets truncated anyway by the buffer
    // clamp below.
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

void LogInit(const std::wstring& process_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_process_name = process_name;
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_process_name.clear();
}

void LogMessage(LogLevel level, std::string_view msg) {
    char line[2048];
    const std::size_t written = FormatLogLine(level,
                                              std::chrono::system_clock::now(),
                                              ::GetCurrentProcessId(),
                                              msg,
                                              line,
                                              sizeof(line));
    if (written == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD written_count = 0;
        ::WriteFile(h, line, static_cast<DWORD>(written), &written_count, nullptr);
    }
    ::OutputDebugStringA(line);
}

}  // namespace leviathan::clipboard_helper
