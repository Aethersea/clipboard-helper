#pragma once
//
// clipboard_ops — Win32 clipboard read/write primitives, callable ONLY from
// the StaWorker thread (because OpenClipboard / SetClipboardData require an
// STA + message pump for OLE marshaling to work end-to-end).
//
// Phase 3a covers CF_UNICODETEXT only. Phase 3b will add image (DIB + PNG
// via WIC); Phase 3c adds delayed rendering via WM_RENDERFORMAT; Phase 4
// adds CF_HDROP and CFSTR_FILEDESCRIPTORW.

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "helper_proto.h"

namespace leviathan::clipboard_helper {

// Snapshot of one of the clipboard slots, decoded into native types that
// the dispatcher can re-serialize as a ClipboardData protobuf.
struct ClipboardSnapshot {
    proto::ClipboardContentType content_type{proto::ClipboardContentType::Unspecified};
    std::wstring                text;       // valid when content_type == Text
    std::vector<std::uint8_t>   image_png;  // valid when content_type == Image
    // Absolute paths of real on-disk files, valid when content_type == Files.
    // Filled by reading CF_HDROP via DragQueryFileW. Virtual files
    // (CFSTR_FILEDESCRIPTORW) belong to Phase 4c and route through a
    // different path.
    std::vector<std::wstring>   file_paths;
};

// Returns true when content_type was populated. False means the clipboard
// has nothing we know how to read (empty, or only formats Phase 3a doesn't
// handle yet).
bool ReadClipboard(HWND owner, ClipboardSnapshot& out);

// Write a UTF-16 string to the clipboard. The OS makes its own copy via
// GlobalAlloc + GlobalLock semantics; we transfer ownership of the HGLOBAL
// to the OS and only fail-cleanup if SetClipboardData returns null.
bool WriteClipboardText(HWND owner, const std::wstring& text);

// Write a PNG-encoded image to the clipboard. The PNG bytes are decoded
// via WIC and re-wrapped as CF_DIBV5 because Win32 native clipboard apps
// generally consume DIB, not PNG. Phase 3b only writes CF_DIBV5; later
// phases may additionally publish a CF_PNG registered format for apps
// that prefer the original PNG bytes.
bool WriteClipboardImagePng(HWND owner, const std::uint8_t* png, std::size_t png_len);

// Write a list of absolute file paths to the clipboard as CF_HDROP.
// The HGLOBAL layout is the DROPFILES header (with pFiles offset + fWide=TRUE)
// followed by each path as UTF-16 with a NUL terminator, ending with a
// final additional NUL ("double-NUL" terminated list per CF_HDROP spec).
bool WriteClipboardFiles(HWND owner, const std::vector<std::wstring>& paths);

// ─── Delayed rendering (Phase 3c) ────────────────────────────────────────
//
// AnnounceDelayedFormatsForType opens the clipboard, empties it, and
// advertises the relevant Win32 format(s) for the requested content type
// with `SetClipboardData(format, NULL)`. The OS records us as the owner;
// when another app calls GetClipboardData on the advertised format, our
// window's WndProc receives WM_RENDERFORMAT and is expected to materialize
// the bytes by calling SetClipboardData(format, hglobal) WITHOUT opening
// the clipboard (the OS already has it open).
//
// For Text: announces CF_UNICODETEXT.
// For Image: announces CF_DIBV5 + CF_DIB (both, so apps that only ask for
//            the older format work too — the WM_RENDERFORMAT handler
//            materializes the same DIB for both).
bool AnnounceDelayedFormatsForType(HWND owner, proto::ClipboardContentType type);

// RenderTextDuringWmRenderFormat writes CF_UNICODETEXT to the clipboard
// WITHOUT opening it. Call ONLY from inside a WM_RENDERFORMAT handler.
bool RenderTextDuringWmRenderFormat(const std::wstring& text);

// RenderImageDuringWmRenderFormat writes CF_DIBV5 (or CF_DIB, depending
// on the requested format) WITHOUT opening the clipboard. Call ONLY from
// inside a WM_RENDERFORMAT handler. `format` is the Win32 clipboard
// format code that the OS asked us to render (CF_DIB or CF_DIBV5).
bool RenderImageDuringWmRenderFormat(UINT format, const std::uint8_t* png, std::size_t png_len);

// RenderFilesDuringWmRenderFormat writes a CF_HDROP HGLOBAL built from a
// newline-separated UTF-8 path list (matching the macOS helper's
// PROVIDE_DATA payload convention for ContentFiles) WITHOUT opening the
// clipboard. Call ONLY from inside a WM_RENDERFORMAT handler.
bool RenderFilesDuringWmRenderFormat(const std::uint8_t* utf8_paths, std::size_t len);

// Convert UTF-8 → UTF-16 with MultiByteToWideChar. Lossy on invalid input.
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& w);

}  // namespace leviathan::clipboard_helper
