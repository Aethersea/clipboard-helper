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
    std::wstring                text;     // valid when content_type == Text
};

// Returns true when content_type was populated. False means the clipboard
// has nothing we know how to read (empty, or only formats Phase 3a doesn't
// handle yet).
bool ReadClipboard(HWND owner, ClipboardSnapshot& out);

// Write a UTF-16 string to the clipboard. The OS makes its own copy via
// GlobalAlloc + GlobalLock semantics; we transfer ownership of the HGLOBAL
// to the OS and only fail-cleanup if SetClipboardData returns null.
bool WriteClipboardText(HWND owner, const std::wstring& text);

// Convert UTF-8 → UTF-16 with MultiByteToWideChar. Lossy on invalid input.
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& w);

}  // namespace leviathan::clipboard_helper
