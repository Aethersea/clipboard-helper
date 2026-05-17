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

#include "wic_image.h"  // kMaxImageDimension (shared cap with the WIC layer)

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

std::vector<std::uint8_t> BuildCfDibV5Payload(int width, int height,
                                              const std::uint8_t* bgra_pixels,
                                              std::size_t pixel_bytes_len) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    if (width > kMaxImageDimension || height > kMaxImageDimension) {
        return {};
    }
    // width is capped at kMaxImageDimension (16384) so width*4 fits in
    // size_t with room to spare on 32-bit and infinitely on 64-bit.
    const std::size_t stride       = static_cast<std::size_t>(width) * 4;
    const std::size_t expected_len = stride * static_cast<std::size_t>(height);
    if (pixel_bytes_len != expected_len) {
        return {};
    }
    if (bgra_pixels == nullptr && expected_len > 0) {
        return {};
    }

    const std::size_t total = sizeof(BITMAPV5HEADER) + expected_len;
    std::vector<std::uint8_t> buf(total, 0);

    auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(buf.data());
    hdr->bV5Size        = sizeof(BITMAPV5HEADER);
    hdr->bV5Width       = width;
    hdr->bV5Height      = -height;  // top-down
    hdr->bV5Planes      = 1;
    hdr->bV5BitCount    = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage   = static_cast<DWORD>(expected_len);
    hdr->bV5RedMask     = 0x00FF0000;
    hdr->bV5GreenMask   = 0x0000FF00;
    hdr->bV5BlueMask    = 0x000000FF;
    hdr->bV5AlphaMask   = 0xFF000000;
    hdr->bV5CSType      = 0x73524742;  // 'sRGB' little-endian FourCC
    hdr->bV5Intent      = LCS_GM_GRAPHICS;

    if (expected_len > 0) {
        std::memcpy(buf.data() + sizeof(BITMAPV5HEADER), bgra_pixels, expected_len);
    }
    return buf;
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
