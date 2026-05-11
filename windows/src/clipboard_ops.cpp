#include "clipboard_ops.h"

#include <cstdio>
#include <cstring>

#include "log.h"
#include "wic_image.h"

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

namespace {

// Read CF_DIBV5 (preferred) or CF_DIB into a BgraImage. CF_DIB is just
// BITMAPINFOHEADER + (optional palette) + pixel rows, with rows bottom-up
// by default and 4-byte-aligned stride. We canonicalize to top-down 32bpp
// BGRA so the WIC encoder can ingest pixels directly.
bool ReadClipboardDibToBgra(BgraImage& out) {
    HANDLE h = ::GetClipboardData(CF_DIBV5);
    UINT format = CF_DIBV5;
    if (h == nullptr) {
        h = ::GetClipboardData(CF_DIB);
        format = CF_DIB;
    }
    if (h == nullptr) return false;

    auto* base = static_cast<const std::uint8_t*>(::GlobalLock(h));
    if (base == nullptr) return false;
    const SIZE_T blob_size = ::GlobalSize(h);

    // Both CF_DIB and CF_DIBV5 begin with biSize so we can read it before
    // committing to a specific header type.
    if (blob_size < sizeof(BITMAPINFOHEADER)) {
        ::GlobalUnlock(h);
        return false;
    }
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > blob_size) {
        ::GlobalUnlock(h);
        return false;
    }

    const LONG raw_w = bih->biWidth;
    const LONG raw_h = bih->biHeight;
    // Reject LONG_MIN explicitly — negating LONG_MIN is signed-overflow UB.
    if (raw_w <= 0 || raw_h == LONG_MIN) {
        ::GlobalUnlock(h);
        return false;
    }
    const bool top_down = raw_h < 0;
    const std::int64_t abs_h = top_down ? -static_cast<std::int64_t>(raw_h)
                                        :  static_cast<std::int64_t>(raw_h);
    if (raw_w > kMaxImageDimension || abs_h > kMaxImageDimension) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "ReadClipboardDibToBgra: rejecting oversized DIB %ldx%lld (cap=%d)",
                      raw_w, static_cast<long long>(abs_h), kMaxImageDimension);
        LH_LOG_WARN(buf);
        ::GlobalUnlock(h);
        return false;
    }
    const int width  = static_cast<int>(raw_w);
    const int height = static_cast<int>(abs_h);
    if (height <= 0) {
        ::GlobalUnlock(h);
        return false;
    }

    // For 24bpp / 32bpp uncompressed (or BI_BITFIELDS) DIBs we can pull the
    // raw pixels and convert. Compressed formats (BI_RLE*, JPEG, PNG via
    // DIB) we skip — uncommon for clipboard contents.
    if (bih->biCompression != BI_RGB && bih->biCompression != BI_BITFIELDS) {
        ::GlobalUnlock(h);
        return false;
    }
    if (bih->biBitCount != 24 && bih->biBitCount != 32) {
        ::GlobalUnlock(h);
        return false;
    }

    // Locate pixel start. Header is biSize bytes; BI_BITFIELDS adds 3
    // RGBQUAD-sized masks (for V5 the masks live inside the header, so
    // skip the supplemental masks).
    std::size_t header_bytes = bih->biSize;
    if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) {
        header_bytes += 3 * sizeof(DWORD);
    }
    if (header_bytes > blob_size) {
        ::GlobalUnlock(h);
        return false;
    }

    const std::uint8_t* src_pixels = base + header_bytes;
    const int  bytes_per_pixel = bih->biBitCount / 8;
    const std::size_t src_stride = (static_cast<std::size_t>(width) * bytes_per_pixel + 3) & ~std::size_t(3);
    const std::size_t needed = src_stride * height;
    if (header_bytes + needed > blob_size) {
        ::GlobalUnlock(h);
        return false;
    }

    out.width  = width;
    out.height = height;
    out.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);

    for (int y = 0; y < height; ++y) {
        const int src_y = top_down ? y : (height - 1 - y);
        const std::uint8_t* src_row = src_pixels + static_cast<std::size_t>(src_y) * src_stride;
        std::uint8_t* dst_row = out.pixels.data() + static_cast<std::size_t>(y) * width * 4;
        if (bytes_per_pixel == 4) {
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(width) * 4);
        } else {
            // 24bpp BGR → 32bpp BGRA with alpha=0xFF.
            for (int x = 0; x < width; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 3 + 0];
                dst_row[x * 4 + 1] = src_row[x * 3 + 1];
                dst_row[x * 4 + 2] = src_row[x * 3 + 2];
                dst_row[x * 4 + 3] = 0xFF;
            }
        }
    }

    ::GlobalUnlock(h);
    (void)format;
    return true;
}

}  // namespace

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

    if (::IsClipboardFormatAvailable(CF_DIB) || ::IsClipboardFormatAvailable(CF_DIBV5)) {
        BgraImage img;
        if (ReadClipboardDibToBgra(img)) {
            if (BgraToPng(img, out.image_png)) {
                out.content_type = proto::ClipboardContentType::Image;
                return true;
            }
        }
    }

    // CF_HDROP and CFSTR_FILEDESCRIPTORW belong to Phase 4.
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

bool WriteClipboardImagePng(HWND owner, const std::uint8_t* png, std::size_t png_len) {
    // Decode PNG → 32bpp BGRA top-down outside the clipboard lock so any
    // WIC error doesn't strand an empty clipboard.
    BgraImage img;
    if (!PngToBgra(png, png_len, img)) {
        return false;
    }

    // Construct a CF_DIBV5 buffer: BITMAPV5HEADER followed by pixel rows.
    // BITMAPV5HEADER carries alpha-channel and color-space hints, so apps
    // that respect transparency (Office, modern Windows shell viewers)
    // composite correctly. The image is laid out top-down (biHeight < 0)
    // and 4-byte-aligned because we're 32bpp.
    const std::size_t stride = static_cast<std::size_t>(img.width) * 4;
    const std::size_t pixel_bytes = stride * img.height;
    const std::size_t total = sizeof(BITMAPV5HEADER) + pixel_bytes;

    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, total);
    if (hglobal == nullptr) {
        LH_LOG_ERROR("GlobalAlloc(CF_DIBV5) failed");
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(::GlobalLock(hglobal));
    if (base == nullptr) {
        ::GlobalFree(hglobal);
        LH_LOG_ERROR("GlobalLock(CF_DIBV5) failed");
        return false;
    }

    auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(base);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->bV5Size        = sizeof(BITMAPV5HEADER);
    hdr->bV5Width       = img.width;
    hdr->bV5Height      = -img.height;  // top-down
    hdr->bV5Planes      = 1;
    hdr->bV5BitCount    = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage   = static_cast<DWORD>(pixel_bytes);
    hdr->bV5RedMask     = 0x00FF0000;
    hdr->bV5GreenMask   = 0x0000FF00;
    hdr->bV5BlueMask    = 0x000000FF;
    hdr->bV5AlphaMask   = 0xFF000000;
    hdr->bV5CSType      = 0x73524742;  // 'sRGB'
    hdr->bV5Intent      = LCS_GM_GRAPHICS;

    std::memcpy(base + sizeof(BITMAPV5HEADER), img.pixels.data(), pixel_bytes);
    ::GlobalUnlock(hglobal);

    // EmptyClipboard + SetClipboardData both require the clipboard to be
    // open; we open it here just before the OS-mutating block so the
    // expensive WIC decode above does not run with the clipboard locked.
    ClipboardScope scope(owner);
    if (!scope) {
        ::GlobalFree(hglobal);
        return false;
    }
    if (!::EmptyClipboard()) {
        ::GlobalFree(hglobal);
        LH_LOG_WARN("EmptyClipboard failed before image SetClipboardData");
        return false;
    }
    if (::SetClipboardData(CF_DIBV5, hglobal) == nullptr) {
        ::GlobalFree(hglobal);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetClipboardData(CF_DIBV5) failed: lastError=%lu",
                      ::GetLastError());
        LH_LOG_ERROR(buf);
        return false;
    }
    return true;
}

// ─── Delayed rendering (Phase 3c) ────────────────────────────────────────

bool AnnounceDelayedFormatsForType(HWND owner, proto::ClipboardContentType type) {
    ClipboardScope scope(owner);
    if (!scope) {
        return false;
    }
    if (!::EmptyClipboard()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "EmptyClipboard failed in AnnounceDelayedFormatsForType: lastError=%lu",
                      ::GetLastError());
        LH_LOG_WARN(buf);
        return false;
    }
    // Helper: SetClipboardData(fmt, NULL) for delayed rendering returns NULL
    // on BOTH success and failure (there is no handle to return). The only
    // reliable success check is GetLastError() == ERROR_SUCCESS after
    // clearing it. Without this we would log spurious failures and the
    // caller would think it had to give up.
    auto announce_delayed = [](UINT fmt, const char* fmt_name) -> bool {
        ::SetLastError(ERROR_SUCCESS);
        ::SetClipboardData(fmt, nullptr);
        const DWORD err = ::GetLastError();
        if (err != ERROR_SUCCESS) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "SetClipboardData(%s, NULL) failed: lastError=%lu",
                          fmt_name, err);
            LH_LOG_WARN(buf);
            return false;
        }
        return true;
    };

    switch (type) {
        case proto::ClipboardContentType::Text:
            return announce_delayed(CF_UNICODETEXT, "CF_UNICODETEXT");
        case proto::ClipboardContentType::Image: {
            const bool dibv5_ok = announce_delayed(CF_DIBV5, "CF_DIBV5");
            // Best-effort CF_DIB for older apps; failure here is non-fatal
            // because CF_DIBV5 already covers modern consumers.
            (void)announce_delayed(CF_DIB, "CF_DIB");
            return dibv5_ok;
        }
        case proto::ClipboardContentType::Files:
        case proto::ClipboardContentType::Unspecified:
        default:
            LH_LOG_WARN("AnnounceDelayedFormatsForType: unsupported type (Phase 4 will add Files)");
            return false;
    }
}

bool RenderTextDuringWmRenderFormat(const std::wstring& text) {
    // The clipboard is already open (the OS opened it before posting
    // WM_RENDERFORMAT); we MUST NOT call OpenClipboard or EmptyClipboard.
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hglobal == nullptr) return false;
    auto* dst = static_cast<wchar_t*>(::GlobalLock(hglobal));
    if (dst == nullptr) {
        ::GlobalFree(hglobal);
        return false;
    }
    std::memcpy(dst, text.data(), text.size() * sizeof(wchar_t));
    dst[text.size()] = L'\0';
    ::GlobalUnlock(hglobal);
    if (::SetClipboardData(CF_UNICODETEXT, hglobal) == nullptr) {
        ::GlobalFree(hglobal);
        return false;
    }
    return true;
}

namespace {

// Build a CF_DIBV5-shaped HGLOBAL from PNG bytes via WIC. Returns nullptr
// on failure. Shared between the Open/Close write path and the
// WM_RENDERFORMAT render path.
HGLOBAL BuildDibV5HGlobalFromPng(const std::uint8_t* png, std::size_t png_len) {
    BgraImage img;
    if (!PngToBgra(png, png_len, img)) {
        return nullptr;
    }
    const std::size_t stride      = static_cast<std::size_t>(img.width) * 4;
    const std::size_t pixel_bytes = stride * img.height;
    const std::size_t total       = sizeof(BITMAPV5HEADER) + pixel_bytes;

    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, total);
    if (hglobal == nullptr) return nullptr;
    auto* base = static_cast<std::uint8_t*>(::GlobalLock(hglobal));
    if (base == nullptr) {
        ::GlobalFree(hglobal);
        return nullptr;
    }
    auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(base);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->bV5Size        = sizeof(BITMAPV5HEADER);
    hdr->bV5Width       = img.width;
    hdr->bV5Height      = -img.height;  // top-down
    hdr->bV5Planes      = 1;
    hdr->bV5BitCount    = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage   = static_cast<DWORD>(pixel_bytes);
    hdr->bV5RedMask     = 0x00FF0000;
    hdr->bV5GreenMask   = 0x0000FF00;
    hdr->bV5BlueMask    = 0x000000FF;
    hdr->bV5AlphaMask   = 0xFF000000;
    hdr->bV5CSType      = 0x73524742;  // 'sRGB'
    hdr->bV5Intent      = LCS_GM_GRAPHICS;
    std::memcpy(base + sizeof(BITMAPV5HEADER), img.pixels.data(), pixel_bytes);
    ::GlobalUnlock(hglobal);
    return hglobal;
}

}  // namespace

bool RenderImageDuringWmRenderFormat(UINT format, const std::uint8_t* png, std::size_t png_len) {
    HGLOBAL hglobal = BuildDibV5HGlobalFromPng(png, png_len);
    if (hglobal == nullptr) return false;
    // CF_DIBV5 is a strict superset of CF_DIB in our top-down 32bpp BGRA
    // layout (the first sizeof(BITMAPINFOHEADER) bytes of BITMAPV5HEADER
    // ARE a valid BITMAPINFOHEADER plus the BI_BITFIELDS masks). Apps
    // that only know CF_DIB will read the prefix happily.
    if (format != CF_DIB && format != CF_DIBV5) {
        ::GlobalFree(hglobal);
        return false;
    }
    if (::SetClipboardData(format, hglobal) == nullptr) {
        ::GlobalFree(hglobal);
        return false;
    }
    return true;
}

}  // namespace leviathan::clipboard_helper
