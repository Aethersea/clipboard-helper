#pragma once

namespace leviathan::clipboard_helper {

// Returns a process exit code to stop, or -1 to continue normal startup.
int HandleUsageArgs(int argc, wchar_t** argv);

}  // namespace leviathan::clipboard_helper
