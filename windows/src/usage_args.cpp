#include "usage_args.h"

#include <cstdio>
#include <cwchar>
#include <string_view>

namespace leviathan::clipboard_helper {
namespace {

// ASCII only, deliberately. `fputws` converts through the stream's current
// locale, and on a default stdout that stops dead at the first character it
// cannot represent -- an em dash here truncated the banner to its first three
// words, which is worse than no banner at all because it still exits 0.
constexpr wchar_t kUsage[] =
    L"leviathan-clipboard-helper - user-session clipboard agent.\n"
    L"Spawned by leviathan and by shen; not meant to be run by hand.\n"
    L"\n"
    L"Usage: leviathan-clipboard-helper [options]\n"
    L"\n"
    L"  --pipe-name=<name>   Named pipe to serve. Defaults to a name derived\n"
    L"                       from the current Windows session, which is what\n"
    L"                       leviathan uses; a second parent in the same\n"
    L"                       session must pass its own or the two collide.\n"
    L"  --parent-pid=<pid>   Parent to watch; the helper exits when it does.\n"
    L"  --parent-handle=<h>  Inherited SYNCHRONIZE handle to the parent, used\n"
    L"                       instead of --parent-pid when the parent runs as\n"
    L"                       SYSTEM and OpenProcess against it would be denied.\n"
    L"  -h, --help           Print this and exit.\n";

// Exit code for a usage error, matching sysexits.h's EX_USAGE — which is what
// the macOS helper's argument parser already returns for the same mistake.
constexpr int kExitUsage = 64;

// Whether `arg` is one of the three value flags this helper understands.
bool IsKnownFlag(std::wstring_view arg) {
    constexpr std::wstring_view kValueFlags[] = {
        L"--pipe-name=",
        L"--parent-pid=",
        L"--parent-handle=",
    };
    for (const std::wstring_view flag : kValueFlags) {
        if (arg.rfind(flag, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool WantsHelp(std::wstring_view arg) {
    return arg == L"--help" || arg == L"-h" || arg == L"/?";
}

}  // namespace

// Answer for `--help`, and refuse anything unrecognised, before any of the
// real startup happens.
//
// Both halves matter, and the second one is why this exists. Argument parsing
// here has always been "scan for the flags I care about and ignore the rest",
// which means an unknown argument did not produce an error — it produced a
// **fully started helper**. A caller that ran `--help` to ask what this binary
// supports therefore got a process that took the default pipe name, lost
// CreateNamedPipeW to whichever helper already held it (ERROR_ACCESS_DENIED),
// retried every second, and never exited. shen probed exactly that way, from
// its Electron main process's UI thread, using a `Command::output()` that
// waits for stdout to reach EOF: the app's window never appeared and its
// DevTools server never answered, because the thread that serves both was
// parked here forever.
//
// Silently accepting nonsense is what turned a wrong question into a hang, so
// the answer is to make the question answerable and the nonsense fatal.
// Returns the process exit code when the program should stop, or -1 to carry
// on. The known-flag list is exhaustive: leviathan passes --parent-pid and
// --parent-handle, shen passes --pipe-name and --parent-pid, and nothing else
// spawns this binary.
int HandleUsageArgs(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (WantsHelp(arg)) {
            std::fputws(kUsage, stdout);
            return 0;
        }
        if (!IsKnownFlag(arg)) {
            std::fwprintf(stderr, L"unknown argument: %ls\n\n", argv[i]);
            std::fputws(kUsage, stderr);
            return kExitUsage;
        }
    }
    return -1;
}

}  // namespace leviathan::clipboard_helper
