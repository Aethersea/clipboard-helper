#pragma once

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

#define LH_LOG_INFO(msg)  ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Info,  (msg))
#define LH_LOG_WARN(msg)  ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Warn,  (msg))
#define LH_LOG_ERROR(msg) ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Error, (msg))
#define LH_LOG_DEBUG(msg) ::leviathan::clipboard_helper::LogMessage(::leviathan::clipboard_helper::LogLevel::Debug, (msg))

}  // namespace leviathan::clipboard_helper
