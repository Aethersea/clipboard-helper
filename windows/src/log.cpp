#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
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

void LogInit(const std::wstring& process_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_process_name = process_name;
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_process_name.clear();
}

void LogMessage(LogLevel level, std::string_view msg) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);

    char timestamp[32];
    std::snprintf(timestamp, sizeof(timestamp),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<long long>(ms));

    char line[2048];
    // snprintf returns the number of characters that WOULD have been written
    // if the buffer were unlimited (excluding the NUL). Clamp to the actual
    // buffer size before passing the byte count to WriteFile, otherwise an
    // oversized log line causes WriteFile to read past `line`.
    const int written_raw = std::snprintf(line, sizeof(line),
                                          "[%s] [%s] [helper:%lu] %.*s\n",
                                          timestamp,
                                          LevelTag(level),
                                          ::GetCurrentProcessId(),
                                          static_cast<int>(msg.size()),
                                          msg.data());
    if (written_raw <= 0) {
        return;
    }
    const int written = std::min<int>(written_raw, static_cast<int>(sizeof(line) - 1));

    std::lock_guard<std::mutex> lock(g_mutex);
    HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD written_count = 0;
        ::WriteFile(h, line, static_cast<DWORD>(written), &written_count, nullptr);
    }
    ::OutputDebugStringA(line);
}

}  // namespace leviathan::clipboard_helper
