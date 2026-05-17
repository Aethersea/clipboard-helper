// UTF-8 ↔ UTF-16 conversion helpers. Lives in its own TU so the codec
// surface (dispatch_codec.cpp) can link it without dragging the rest of
// clipboard_ops.cpp's OLE / shellapi dependencies into the test binary.
//
// Declarations stay in clipboard_ops.h to keep existing call sites
// unchanged; only the implementations relocate here.

#include "clipboard_ops.h"

#include <windows.h>

#include <cstddef>

namespace leviathan::clipboard_helper {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wlen);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace leviathan::clipboard_helper
