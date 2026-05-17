#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace leviathan::clipboard_helper {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

void LogInit(const std::wstring& process_name);
void LogShutdown();

void LogMessage(LogLevel level, std::string_view msg);

// FormatLogLine builds the textual log line that LogMessage would emit,
// into the caller-supplied buffer. Pure: no I/O, no clock reads — `now`
// and `pid` are passed in so tests can pin them.
//
// Layout: "[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [helper:PID] MSG\n"
//
// Returns the number of bytes written (excluding the trailing NUL),
// clamped to out_size - 1. Returns 0 on snprintf failure or if out_size
// is zero. The buffer is always NUL-terminated on success.
std::size_t FormatLogLine(LogLevel                              level,
                          std::chrono::system_clock::time_point now,
                          std::uint32_t                         pid,
                          std::string_view                      msg,
                          char*                                 out,
                          std::size_t                           out_size);

#define LH_LOG_INFO(msg)  ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Info,  (msg))
#define LH_LOG_WARN(msg)  ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Warn,  (msg))
#define LH_LOG_ERROR(msg) ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Error, (msg))
#define LH_LOG_DEBUG(msg) ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Debug, (msg))

}  // namespace leviathan::clipboard_helper
