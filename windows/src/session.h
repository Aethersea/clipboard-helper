#pragma once

#include <windows.h>

#include <string>

namespace leviathan::clipboard_helper {

DWORD GetCurrentSessionId();

std::wstring DefaultPipeName(DWORD session_id);

}  // namespace leviathan::clipboard_helper
