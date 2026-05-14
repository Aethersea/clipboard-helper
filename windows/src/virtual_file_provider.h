#pragma once
//
// virtual_file_provider — IDataObject server for delivering remote-clipboard
// files via OLE virtual files (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS).
//
// Replaces the CF_HDROP delayed-rendering path for content_type=Files. With
// virtual files the OS pulls bytes lazily via IStream::Read, so the helper
// never blocks the shell waiting for shen to download a remote file — the
// behavior that made Explorer freeze and the desktop go black with the old
// 120 s WM_RENDERFORMAT wait. See plan-virtual-file-clipboard.md.
//
// Lifetime:
//   * CreateVirtualClipboardDataObject returns an IDataObject* with
//     refcount=1. Pass it to OleSetClipboard (which AddRefs) and Release
//     your reference, OR call Release() yourself to dispose.
//   * The IDataObject hands out fresh VirtualFileStream instances for
//     each CFSTR_FILECONTENTS GetData. Consumers Release() them after use.
//   * `provider` is borrowed — the caller must keep it alive at least until
//     the IDataObject AND any outstanding IStream proxies are released.
//
// Threading:
//   * The IDataObject is created on, and intended to live on, the helper's
//     single STA worker thread. COM marshaling routes incoming GetData /
//     IStream::Read calls from external apartments back to the STA thread.
//   * ChunkProvider::FetchChunk MUST therefore be safe to call from the
//     STA thread while it is mid-COM-call; in practice the implementation
//     pumps messages via MsgWaitForMultipleObjects so a nested OLE
//     marshaled call cannot deadlock.
//   * If we later observe re-entrancy problems we will install
//     CoCreateFreeThreadedMarshaler so calls run on the inbound RPC
//     thread instead. Phase 5 risk in the plan.

#include <windows.h>
#include <ole2.h>

#include <cstdint>
#include <string>
#include <vector>

namespace leviathan::clipboard_helper {

// Per-file metadata used to populate the CFSTR_FILEDESCRIPTORW group.
//
// `name`         — file path the consumer sees. May include backslash-
//                  separated subdirectories when copying a folder tree;
//                  Windows' virtual-file shell consumers split on '\\'
//                  to build the destination layout. Truncated to MAX_PATH-1
//                  chars when copied into FILEDESCRIPTORW.cFileName.
// `size`         — fills FILEDESCRIPTORW.nFileSize{Low,High}. 0 for dirs.
// `file_id`      — opaque token forwarded to ChunkProvider::FetchChunk.
//                  Matches FileMetadata.file_id from the announcement
//                  protobuf, so the parent can resolve it back to the
//                  upstream WebRTC source.
// `is_directory` — selects FILE_ATTRIBUTE_DIRECTORY vs FILE_ATTRIBUTE_NORMAL
//                  in dwFileAttributes. Directories should still appear
//                  with their entire descendant set as additional specs
//                  (FILEGROUPDESCRIPTOR enumerates files; the shell
//                  reconstructs the tree from sibling entries' relative
//                  paths).
struct VirtualFileSpec {
    std::wstring  name;
    std::uint64_t size{0};
    std::string   file_id;
    bool          is_directory{false};
};

// Abstract callback the IDataObject uses to pull file bytes from upstream
// (in practice the parent process via the dispatcher's PipeServer round-
// trip).
//
// FetchChunk MUST be safe to call from any thread COM marshals into us
// (typically the STA worker thread). Implementations are expected to
// block until the bytes are available, propagate cancellation via
// E_ABORT, and surface ownership-change races as STG_E_REVERTED so the
// shell aborts the in-flight copy cleanly.
class ChunkProvider {
public:
    virtual ~ChunkProvider() = default;

    // Synchronously fetch up to `size` bytes starting at `offset` for the
    // file identified by (`transfer_id`, `file_id`). On success `out_data`
    // is resized to the number of bytes actually read — may be less than
    // `size` at EOF. `out_is_last` reflects EOF as observed by the
    // upstream producer; this is advisory (IStream callers also see
    // short reads via pcbRead).
    //
    // The `transfer_id` is carried alongside `file_id` so concurrent
    // announcements don't have their chunk requests collide on file_id
    // alone (file_id is opaque and only unique within an announcement).
    //
    // Return values:
    //   S_OK            — bytes available in out_data (may be zero at EOF)
    //   E_ABORT         — caller-initiated cancellation
    //   STG_E_REVERTED  — clipboard ownership rotated, stream stale
    //   E_FAIL          — transport / parent error
    virtual HRESULT FetchChunk(const std::string&         transfer_id,
                               const std::string&         file_id,
                               std::uint64_t              offset,
                               std::uint32_t              size,
                               std::vector<std::uint8_t>& out_data,
                               bool&                      out_is_last) = 0;
};

// CFSTR_PERFORMEDDROPEFFECT clipboard-format atom (lazy-registered).
// CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS already have getters in
// clipboard_ops.h (GetCfFileDescriptor / GetCfFileContents) — reuse those.
UINT GetCfPerformedDropEffect();

// Create an IDataObject server that advertises CFSTR_FILEDESCRIPTORW
// (metadata for `specs`) and CFSTR_FILECONTENTS / TYMED_ISTREAM (one
// VirtualFileStream per file, lazily produced on demand against
// `provider`).
//
// `transfer_id` is the announcement-scoped identifier passed back to
// the ChunkProvider for every FetchChunk; the data object captures it
// at construction so concurrent announcements cannot cross-talk.
//
// `provider` is borrowed; it must outlive the returned IDataObject and
// any outstanding IStream proxies handed out by it.
//
// The returned interface starts with refcount=1. Caller is responsible
// for the initial Release (OleSetClipboard takes its own reference when
// you hand the pointer off).
HRESULT CreateVirtualClipboardDataObject(std::vector<VirtualFileSpec> specs,
                                         std::string                  transfer_id,
                                         ChunkProvider*               provider,
                                         IDataObject**                out_object);

}  // namespace leviathan::clipboard_helper
