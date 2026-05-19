#include "clipboard_ops.h"

#include <ole2.h>        // OleGetClipboard, IDataObject, STGMEDIUM
#include <shellapi.h>    // DragQueryFileW, HDROP, DROPFILES
#include <shlobj.h>      // FILEDESCRIPTORW, FILEGROUPDESCRIPTORW, CFSTR_*

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "wic_image.h"

namespace leviathan::clipboard_helper {

namespace {

// RAII wrapper around OpenClipboard / CloseClipboard. OpenClipboard contends
// with whatever app currently owns the clipboard, so we retry briefly when
// the OS reports another locker (typical when a paste sequence is in flight).
//
// The thread-local re-entrancy guard (OsClipboardHeldGuard) lives in
// clipboard_format.cpp; we query it via IsOsClipboardHeldByThisThread()
// rather than touching the thread_local directly so the implementation
// can stay testable in isolation.
class ClipboardScope {
public:
    explicit ClipboardScope(HWND owner) {
        if (IsOsClipboardHeldByThisThread()) {
            // We are nested inside a WM_RENDERFORMAT / WM_RENDERALLFORMATS
            // dispatch. The OS already has the clipboard open on behalf of
            // the paste-target app; retrying OpenClipboard 10×15 ms would
            // just delay the inevitable failure and stall the RunSync the
            // pipe thread is waiting on. Fail fast — callers translate
            // the false return into an ERROR frame and the pipe thread
            // continues serving the next request.
            LH_LOG_WARN("ClipboardScope: skipped — OS holds clipboard for nested render-format dispatch");
            return;
        }
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

    // File formats must be probed BEFORE text/image.  When Explorer copies a
    // file it advertises *both* CF_HDROP and CF_UNICODETEXT (the full path as
    // text) on the clipboard, so a naive "text first" lookup misclassifies
    // every file copy as a text copy and the parent never receives a Files
    // CLIPBOARD_CHANGED frame.  The macOS helper has the same ordering rule;
    // see PasteboardManager.swift:readPasteboard.

    // CF_HDROP — physical-file copies (Explorer, file-manager apps).
    if (::IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE h = ::GetClipboardData(CF_HDROP);
        if (h != nullptr) {
            HDROP drop = static_cast<HDROP>(::GlobalLock(h));
            if (drop != nullptr) {
                // DragQueryFileW with iFile=0xFFFFFFFF returns the count.
                const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                out.file_paths.reserve(count);
                for (UINT i = 0; i < count; ++i) {
                    const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                    if (len == 0) continue;
                    std::wstring path(static_cast<std::size_t>(len), L'\0');
                    // The returned size excludes the NUL but DragQueryFileW
                    // needs space for it, hence len+1.
                    const UINT got = ::DragQueryFileW(drop, i, path.data(),
                                                      static_cast<UINT>(len + 1));
                    if (got == 0) continue;
                    path.resize(got);
                    out.file_paths.push_back(std::move(path));
                }
                ::GlobalUnlock(h);
                if (!out.file_paths.empty()) {
                    out.content_type = proto::ClipboardContentType::Files;
                    return true;
                }
            }
        }
    }

    // CFSTR_FILEDESCRIPTORW (virtual files: Outlook attachments, zip
    // entries). The advertised format is a registered (non-built-in)
    // clipboard format, so IsClipboardFormatAvailable + GetClipboardData
    // are usable, but the producer typically exposes the bytes via
    // CFSTR_FILECONTENTS through IDataObject only. Use the OLE entry
    // point for both detection and later chunk fetches.
    if (::IsClipboardFormatAvailable(GetCfFileDescriptor())) {
        if (ReadVirtualFiles(out)) {
            return true;
        }
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

    // BITMAPV5HEADER + pixel rows. The pure layout is in clipboard_format.cpp
    // so it can be unit-tested without WIC. Caller-side responsibility:
    // copy the buffer into an HGLOBAL the OS can own.
    const std::vector<std::uint8_t> payload = BuildCfDibV5Payload(
        img.width, img.height, img.pixels.data(), img.pixels.size());
    if (payload.empty()) {
        LH_LOG_ERROR("BuildCfDibV5Payload rejected the decoded image");
        return false;
    }

    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, payload.size());
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
    std::memcpy(base, payload.data(), payload.size());
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

bool WriteClipboardFiles(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;

    // Build the DROPFILES + UTF-16 path list in a plain buffer first; the
    // pure layout lives in clipboard_format.cpp so it can be unit-tested.
    const std::vector<std::uint8_t> payload = BuildCfHdropPayload(paths);
    if (payload.empty()) return false;

    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, payload.size());
    if (hglobal == nullptr) {
        LH_LOG_ERROR("GlobalAlloc(CF_HDROP) failed");
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(::GlobalLock(hglobal));
    if (base == nullptr) {
        ::GlobalFree(hglobal);
        LH_LOG_ERROR("GlobalLock(CF_HDROP) failed");
        return false;
    }
    std::memcpy(base, payload.data(), payload.size());
    ::GlobalUnlock(hglobal);

    ClipboardScope scope(owner);
    if (!scope) {
        ::GlobalFree(hglobal);
        return false;
    }
    if (!::EmptyClipboard()) {
        ::GlobalFree(hglobal);
        LH_LOG_WARN("EmptyClipboard failed before CF_HDROP SetClipboardData");
        return false;
    }
    if (::SetClipboardData(CF_HDROP, hglobal) == nullptr) {
        ::GlobalFree(hglobal);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetClipboardData(CF_HDROP) failed: lastError=%lu",
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
            // Phase 2 cutover: Files content is no longer published via
            // SetClipboardData(CF_HDROP, NULL) — it goes through
            // OleSetClipboard with a virtual-file IDataObject in
            // Dispatcher::HandleAnnounceDelayed instead, so this function
            // is never called for Files. Logged in case a regression
            // re-routes Files here.
            LH_LOG_WARN("AnnounceDelayedFormatsForType: Files should be served by OleSetClipboard");
            return false;
        case proto::ClipboardContentType::Unspecified:
        default:
            LH_LOG_WARN("AnnounceDelayedFormatsForType: unsupported type");
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
    const std::vector<std::uint8_t> payload = BuildCfDibV5Payload(
        img.width, img.height, img.pixels.data(), img.pixels.size());
    if (payload.empty()) return nullptr;

    HGLOBAL hglobal = ::GlobalAlloc(GMEM_MOVEABLE, payload.size());
    if (hglobal == nullptr) return nullptr;
    auto* base = static_cast<std::uint8_t*>(::GlobalLock(hglobal));
    if (base == nullptr) {
        ::GlobalFree(hglobal);
        return nullptr;
    }
    std::memcpy(base, payload.data(), payload.size());
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

// ─── Virtual files via CFSTR_FILEDESCRIPTORW (Phase 4c) ──────────────────

UINT GetCfFileDescriptor() {
    static UINT cf = ::RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    return cf;
}

UINT GetCfFileContents() {
    static UINT cf = ::RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    return cf;
}

bool ReadVirtualFiles(ClipboardSnapshot& out) {
    out.virtual_files.clear();
    out.virtual_data_object = nullptr;

    if (IsOsClipboardHeldByThisThread()) {
        // Same fail-fast rationale as ClipboardScope: OleGetClipboard
        // inside an active WM_RENDERFORMAT dispatch is unsupported and
        // would serialize the STA against itself.
        LH_LOG_WARN("ReadVirtualFiles: skipped — OS holds clipboard for nested render-format dispatch");
        return false;
    }

    IDataObject* obj = nullptr;
    HRESULT hr = ::OleGetClipboard(&obj);
    if (FAILED(hr) || obj == nullptr) {
        return false;
    }

    const UINT cf_descriptor = GetCfFileDescriptor();
    if (cf_descriptor == 0) {
        obj->Release();
        return false;
    }

    FORMATETC fe{};
    fe.cfFormat = static_cast<CLIPFORMAT>(cf_descriptor);
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex   = -1;
    fe.tymed    = TYMED_HGLOBAL;

    STGMEDIUM sm{};
    hr = obj->GetData(&fe, &sm);
    if (FAILED(hr) || sm.tymed != TYMED_HGLOBAL || sm.hGlobal == nullptr) {
        obj->Release();
        return false;
    }

    auto* fg = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(sm.hGlobal));
    if (fg == nullptr) {
        ::ReleaseStgMedium(&sm);
        obj->Release();
        return false;
    }

    // Defensive bounds + cap on the foreign IDataObject's FILEGROUPDESCRIPTORW.
    // Two attacks the producer could (deliberately or via memory corruption)
    // mount:
    //   1. cItems advertises N entries but the HGLOBAL is sized for only K<N
    //      → reading fg->fgd[i] for i in [K..N) walks off the end of the
    //      allocation. UB / process crash.
    //   2. cItems is enormous (UINT_MAX) → reserve() triggers a multi-GiB
    //      allocation and we OOM before the bounds check kicks in.
    // Cap symmetrically with the WRITE side (see virtual_file_provider.cpp's
    // kMaxVirtualFileSpecs = 100,000) and verify the HGLOBAL actually
    // contains the trailing FILEDESCRIPTORW slots cItems claims.
    constexpr UINT kMaxInboundVirtualFiles = 100'000;
    const SIZE_T blob_size = ::GlobalSize(sm.hGlobal);
    UINT count = fg->cItems;
    if (count > kMaxInboundVirtualFiles) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ReadVirtualFiles: rejecting cItems=%u (cap=%u) — upstream malformed",
                      count, kMaxInboundVirtualFiles);
        LH_LOG_WARN(buf);
        ::GlobalUnlock(sm.hGlobal);
        ::ReleaseStgMedium(&sm);
        obj->Release();
        return false;
    }
    // FILEGROUPDESCRIPTORW already includes one FILEDESCRIPTORW slot; the
    // remaining (count-1) slots tail the struct in the HGLOBAL.
    if (count > 0) {
        const SIZE_T needed = sizeof(FILEGROUPDESCRIPTORW) +
                              static_cast<SIZE_T>(count - 1) * sizeof(FILEDESCRIPTORW);
        if (needed > blob_size) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "ReadVirtualFiles: cItems=%u requires %zu bytes but HGLOBAL is %zu",
                          count, static_cast<std::size_t>(needed),
                          static_cast<std::size_t>(blob_size));
            LH_LOG_WARN(buf);
            ::GlobalUnlock(sm.hGlobal);
            ::ReleaseStgMedium(&sm);
            obj->Release();
            return false;
        }
    }

    out.virtual_files.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const FILEDESCRIPTORW& fd = fg->fgd[i];
        VirtualFileEntry e{};
        // fd.cFileName is documented as NUL-terminated within MAX_PATH
        // wchars. wstring's wchar_t* constructor stops at the first L'\0',
        // which protects us if the producer forgot the NUL — the C++
        // string ctor would walk to the next L'\0' in the HGLOBAL, but
        // FILEDESCRIPTORW.cFileName is MAX_PATH wchars by ABI so this
        // can't escape into adjacent slot memory.
        e.name = fd.cFileName;
        if (fd.dwFlags & FD_FILESIZE) {
            ULARGE_INTEGER sz{};
            sz.LowPart  = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            e.size = sz.QuadPart;
        }
        e.lindex = i;
        out.virtual_files.push_back(std::move(e));
    }

    ::GlobalUnlock(sm.hGlobal);
    ::ReleaseStgMedium(&sm);

    // Hand the IDataObject pointer ownership to the snapshot — the caller
    // (Dispatcher) is responsible for ReleaseDataObject when superseded.
    out.virtual_data_object = obj;
    out.content_type = proto::ClipboardContentType::Files;
    return true;
}

bool ReadVirtualFileChunk(void* data_object, std::uint32_t lindex,
                          std::uint64_t offset, std::uint32_t size,
                          std::vector<std::uint8_t>& out_data,
                          bool& out_is_last) {
    out_data.clear();
    out_is_last = false;
    if (data_object == nullptr || size == 0) return false;

    const UINT cf_contents = GetCfFileContents();
    if (cf_contents == 0) return false;

    auto* obj = static_cast<IDataObject*>(data_object);

    // Prefer TYMED_ISTREAM (random access; the producer typically backs
    // these by a stream). Fall back to TYMED_HGLOBAL when the producer
    // only supplies an HGLOBAL blob. TYMED_FILE (path to a temp file on
    // disk) is the rarest case and not handled here yet.
    FORMATETC fe{};
    fe.cfFormat = static_cast<CLIPFORMAT>(cf_contents);
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex   = static_cast<LONG>(lindex);
    fe.tymed    = TYMED_ISTREAM | TYMED_HGLOBAL;

    STGMEDIUM sm{};
    HRESULT hr = obj->GetData(&fe, &sm);
    if (FAILED(hr)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "GetData(CFSTR_FILECONTENTS, lindex=%u) failed: hr=0x%08lx",
                      lindex, hr);
        LH_LOG_WARN(buf);
        return false;
    }

    bool ok = false;
    if (sm.tymed == TYMED_ISTREAM && sm.pstm != nullptr) {
        // Seek to offset, read `size` bytes. We rely on the producer's
        // IStream being seekable; for the few that aren't this fails
        // cleanly with E_NOTIMPL and the parent retries.
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(offset);
        ULARGE_INTEGER pos{};
        hr = sm.pstm->Seek(li, STREAM_SEEK_SET, &pos);
        if (SUCCEEDED(hr)) {
            out_data.resize(size);
            ULONG got = 0;
            hr = sm.pstm->Read(out_data.data(), size, &got);
            if (SUCCEEDED(hr)) {
                out_data.resize(got);
                out_is_last = (got < size);
                ok = true;
            }
        }
    } else if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal != nullptr) {
        const SIZE_T total = ::GlobalSize(sm.hGlobal);
        if (offset < total) {
            auto* src = static_cast<const std::uint8_t*>(::GlobalLock(sm.hGlobal));
            if (src != nullptr) {
                const std::size_t avail = static_cast<std::size_t>(total - offset);
                const std::size_t take  = std::min<std::size_t>(avail, size);
                out_data.assign(src + offset, src + offset + take);
                ::GlobalUnlock(sm.hGlobal);
                out_is_last = (offset + take >= total);
                ok = true;
            }
        } else {
            // offset past EOF — empty chunk, is_last=true.
            out_is_last = true;
            ok = true;
        }
    } else {
        LH_LOG_WARN("CFSTR_FILECONTENTS returned an unsupported TYMED");
    }

    ::ReleaseStgMedium(&sm);
    return ok;
}

void ReleaseDataObject(void* data_object) {
    if (data_object == nullptr) return;
    auto* obj = static_cast<IDataObject*>(data_object);
    obj->Release();
}

void AddRefDataObject(void* data_object) {
    if (data_object == nullptr) return;
    auto* obj = static_cast<IDataObject*>(data_object);
    obj->AddRef();
}

}  // namespace leviathan::clipboard_helper
