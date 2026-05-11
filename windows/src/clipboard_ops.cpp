#include "clipboard_ops.h"

#include <cstdio>
#include <cstring>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

// RAII wrapper around OpenClipboard / CloseClipboard. OpenClipboard contends
// with whatever app currently owns the clipboard, so we retry briefly when
// the OS reports another locker (typical when a paste sequence is in flight).
class ClipboardScope {
public:
    explicit ClipboardScope(HWND owner) {
        constexpr int kMaxAttempts = 10;
        for (int i = 0; i < kMaxAttempts; ++i) {
            if (::OpenClipboard(owner)) {
                opened_ = true;
                return;
            }
            ::Sleep(15);
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "OpenClipboard failed: lastError=%lu", ::GetLastError());
        LH_LOG_WARN(buf);
    }
    ~ClipboardScope() {
        if (opened_) {
            ::CloseClipboard();
        }
    }
    explicit operator bool() const { return opened_; }

    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;

private:
    bool opened_{false};
};

}  // namespace

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

bool ReadClipboard(HWND owner, ClipboardSnapshot& out) {
    ClipboardScope scope(owner);
    if (!scope) {
        return false;
    }

    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
        if (h != nullptr) {
            auto* p = static_cast<const wchar_t*>(::GlobalLock(h));
            if (p != nullptr) {
                // Trust the OS NUL-termination. wcslen is safe because the
                // clipboard always stores a NUL after the user data.
                const std::size_t n = ::wcslen(p);
                out.text.assign(p, p + n);
                out.content_type = proto::ClipboardContentType::Text;
                ::GlobalUnlock(h);
                return true;
            }
        }
    }
    // Other formats (CF_DIB, CF_HDROP, CFSTR_FILEDESCRIPTORW, …) belong
    // to Phase 3b/4. Return false and let the dispatcher decide what to
    // do (typically: send an empty ClipboardData, equivalent to "nothing
    // we know how to ship").
    return false;
}

bool WriteClipboardText(HWND owner, const std::wstring& text) {
    ClipboardScope scope(owner);
    if (!scope) {
        return false;
    }

    if (!::EmptyClipboard()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "EmptyClipboard failed: lastError=%lu", ::GetLastError());
        LH_LOG_WARN(buf);
        return false;
    }

    // Include the trailing NUL so other apps' GetClipboardData consumers
    // don't read past the end. GMEM_MOVEABLE is the canonical mode for
    // text clipboard handles per the Win32 docs.
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hglobal == nullptr) {
        LH_LOG_ERROR("GlobalAlloc(CF_UNICODETEXT) failed");
        return false;
    }
    auto* dst = static_cast<wchar_t*>(::GlobalLock(hglobal));
    if (dst == nullptr) {
        ::GlobalFree(hglobal);
        LH_LOG_ERROR("GlobalLock(CF_UNICODETEXT) failed");
        return false;
    }
    std::memcpy(dst, text.data(), text.size() * sizeof(wchar_t));
    dst[text.size()] = L'\0';
    ::GlobalUnlock(hglobal);

    if (::SetClipboardData(CF_UNICODETEXT, hglobal) == nullptr) {
        // SetClipboardData takes ownership only on success; on failure we
        // must free our HGLOBAL or it leaks.
        ::GlobalFree(hglobal);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetClipboardData(CF_UNICODETEXT) failed: lastError=%lu",
                      ::GetLastError());
        LH_LOG_ERROR(buf);
        return false;
    }
    return true;
}

}  // namespace leviathan::clipboard_helper
