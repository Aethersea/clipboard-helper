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
//
// For Files content the snapshot can carry two flavors:
//   * Physical files (CF_HDROP) — file_paths has absolute Win32 paths
//     and virtual_files is empty.
//   * Virtual files (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS) —
//     virtual_files holds per-file metadata, file_paths is empty. The
//     IDataObject pointer that owns the file streams is parked in
//     virtual_data_object so a later FILE_CHUNK_REQUEST can pull bytes
//     via IDataObject::GetData(CFSTR_FILECONTENTS, lindex).
struct VirtualFileEntry {
    std::wstring  name;
    std::uint64_t size{0};
    std::uint32_t lindex{0};  // formatetc.lindex for CFSTR_FILECONTENTS
};

struct ClipboardSnapshot {
    proto::ClipboardContentType  content_type{proto::ClipboardContentType::Unspecified};
    std::wstring                 text;       // valid when content_type == Text
    std::vector<std::uint8_t>    image_png;  // valid when content_type == Image
    std::vector<std::wstring>    file_paths;
    std::vector<VirtualFileEntry> virtual_files;
    // Caller-owned IDataObject reference (AddRef'd on success). The
    // dispatcher takes ownership and Releases when superseded. Read code
    // sets this *only* when virtual_files is non-empty.
    void*                        virtual_data_object{nullptr};
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

// ─── Virtual files via CFSTR_FILEDESCRIPTORW (Phase 4c) ──────────────────
//
// One-time registration of the CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS
// clipboard format atoms. The atoms are process-wide, so we cache them
// after the first lookup. Safe to call multiple times from the STA thread.
UINT GetCfFileDescriptor();
UINT GetCfFileContents();

// Read virtual files (drag-from-Outlook, zip preview, etc.) off the
// current clipboard via OleGetClipboard + GetData(CFSTR_FILEDESCRIPTORW).
// On success, out.virtual_files is populated with per-file metadata and
// out.virtual_data_object holds an AddRef'd IDataObject pointer the caller
// must Release when finished. Returns false when the clipboard doesn't
// advertise CFSTR_FILEDESCRIPTORW or the parse fails.
//
// Must be called on the STA worker thread (uses OleGetClipboard).
bool ReadVirtualFiles(ClipboardSnapshot& out);

// Read one chunk of a virtual file via IDataObject::GetData(
// CFSTR_FILECONTENTS, lindex). `data_object` is the pointer the snapshot
// returned; `lindex` matches VirtualFileEntry::lindex. On success out_data
// is resized to the chunk length actually read and out_is_last reflects
// EOF.
//
// Must be called on the STA worker thread.
bool ReadVirtualFileChunk(void* data_object, std::uint32_t lindex,
                          std::uint64_t offset, std::uint32_t size,
                          std::vector<std::uint8_t>& out_data,
                          bool& out_is_last);

// Release an IDataObject pointer previously held in
// ClipboardSnapshot::virtual_data_object. Wrapper so callers don't have
// to include <unknwn.h> just to call Release().
void ReleaseDataObject(void* data_object);

// AddRef an IDataObject pointer. Used by the dispatcher to pin a virtual
// data object snapshot for the duration of a cross-thread RunSync call so
// the STA worker can't release it out from under us.
void AddRefDataObject(void* data_object);

// ─── WM_RENDERFORMAT re-entrancy guard ───────────────────────────────────
//
// While the STA worker is dispatching WM_RENDERFORMAT or WM_RENDERALLFORMATS,
// the OS already holds the clipboard open on behalf of the paste-target app.
// Any nested OpenClipboard / OleGetClipboard call from a RunSync'd work item
// (SET_CLIPBOARD, GET_CLIPBOARD, ANNOUNCE_DELAYED) would either burn ~150 ms
// in OpenClipboard retries OR — worse — succeed and corrupt the in-flight
// delayed-render handoff.
//
// The STA worker wraps its cb() invocation in OsClipboardHeldGuard so any
// clipboard helper called from the same thread fails fast instead of retrying.
// This lets the pipe thread's RunSync return promptly with an error rather
// than wedging for up to kRenderTimeoutMs.

class OsClipboardHeldGuard {
public:
    OsClipboardHeldGuard();
    ~OsClipboardHeldGuard();
    OsClipboardHeldGuard(const OsClipboardHeldGuard&) = delete;
    OsClipboardHeldGuard& operator=(const OsClipboardHeldGuard&) = delete;
};

// Returns true iff the current thread is inside an OsClipboardHeldGuard
// scope (i.e. inside a WM_RENDERFORMAT / WM_RENDERALLFORMATS dispatch).
bool IsOsClipboardHeldByThisThread();

// Convert UTF-8 → UTF-16 with MultiByteToWideChar. Lossy on invalid input.
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& w);

}  // namespace leviathan::clipboard_helper
