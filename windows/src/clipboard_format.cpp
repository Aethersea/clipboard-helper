// clipboard_format — OLE-free pure helpers for Windows clipboard format
// byte layouts and the re-entrancy guard used by the STA worker.
//
// Lives in its own TU so the test binary can exercise these helpers
// without linking the OLE / WIC / shellapi-heavy clipboard_ops.cpp.
// Declarations stay in clipboard_ops.h so existing call sites are
// unchanged.

#include "clipboard_ops.h"

#include <windows.h>
#include <shellapi.h>   // DROPFILES (with WIN32_LEAN_AND_MEAN we still need shlobj.h)
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace leviathan::clipboard_helper {

namespace {

// Counter — not bool — so nested OsClipboardHeldGuard scopes (the STA
// worker enters one outer guard for WM_RENDERFORMAT, then any inner
// helper that takes its own guard symmetrically unwinds without
// clobbering the outer state) reference-count correctly.
thread_local int tls_os_clipboard_held = 0;

}  // namespace

OsClipboardHeldGuard::OsClipboardHeldGuard() {
    ++tls_os_clipboard_held;
}

OsClipboardHeldGuard::~OsClipboardHeldGuard() {
    --tls_os_clipboard_held;
}

bool IsOsClipboardHeldByThisThread() {
    return tls_os_clipboard_held != 0;
}

std::vector<std::uint8_t> BuildCfHdropPayload(const std::vector<std::wstring>& paths) {
    if (paths.empty()) {
        return {};
    }

    // CF_HDROP wire layout:
    //   DROPFILES { pFiles=sizeof(DROPFILES), fWide=TRUE, pt={0,0}, fNC=0 }
    //   <path0> NUL <path1> NUL ... <pathN-1> NUL NUL
    //
    // Each path gets exactly one trailing NUL; the buffer is terminated
    // by one additional NUL ("double-NUL" per CF_HDROP spec). DragQueryFileW
    // walks the buffer by that convention.
    std::size_t total_wchars = 0;
    for (const auto& p : paths) total_wchars += p.size() + 1;  // +1 for per-path NUL
    total_wchars += 1;  // final double-NUL terminator

    const std::size_t total_bytes = sizeof(DROPFILES) + total_wchars * sizeof(wchar_t);
    std::vector<std::uint8_t> buf(total_bytes, 0);

    auto* df = reinterpret_cast<DROPFILES*>(buf.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;

    auto* dst = reinterpret_cast<wchar_t*>(buf.data() + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.data(), p.size() * sizeof(wchar_t));
        dst += p.size();
        *dst++ = L'\0';
    }
    *dst = L'\0';

    return buf;
}

}  // namespace leviathan::clipboard_helper
