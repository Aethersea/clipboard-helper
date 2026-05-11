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

// Convert UTF-8 → UTF-16 with MultiByteToWideChar. Lossy on invalid input.
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& w);

}  // namespace leviathan::clipboard_helper
