#include "session.h"

#include <string>

namespace leviathan::clipboard_helper {

DWORD GetCurrentSessionId() {
    DWORD session_id = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id)) {
        session_id = 0;
    }
    return session_id;
}

std::wstring DefaultPipeName(DWORD session_id) {
    return L"\\\\.\\pipe\\leviathan-clipboard-" + std::to_wstring(session_id);
}

}  // namespace leviathan::clipboard_helper
