#pragma once

#include <windows.h>
#include <ole2.h>   // IDataObject — outbound_data_object_ pointee

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "helper_proto.h"
#include "virtual_file_provider.h"

namespace leviathan::clipboard_helper {

class StaWorker;
class PipeServer;
// Forward declaration; full definition in dispatch.cpp. The provider is
// the ChunkProvider impl handed to a VirtualClipboardDataObject created
// when shen sends ANNOUNCE_DELAYED with content_type=Files — it round-
// trips FILE_CHUNK_REQUEST / FILE_CHUNK_DATA through PipeServer.
class DispatcherChunkProvider;

// Dispatcher owns the wiring between inbound HelperMessage frames (parsed by
// HandleHelperMessage) and the STA worker that runs the actual clipboard
// operations. It also pushes unsolicited HELPER_MESSAGE_TYPE_CLIPBOARD_CHANGED
// frames out through the pipe whenever the STA worker observes a clipboard
// sequence change.
//
// Lifetime: constructed by main.cpp after both StaWorker and PipeServer are
// ready. The Dispatcher does not own either dependency.
class Dispatcher {
public:
    Dispatcher(StaWorker* sta, PipeServer* pipe);
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Inbound frame handler installed on PipeServer.
    std::vector<std::uint8_t> Handle(const std::vector<std::uint8_t>& request);

    // Frame to push to a new client right after they connect (READY).
    std::vector<std::uint8_t> OnConnect();

    // Reads the current clipboard via the STA worker and, if anything readable
    // is present, ships HELPER_MESSAGE_TYPE_CLIPBOARD_CHANGED back through the
    // pipe. Invoked from the STA thread on WM_CLIPBOARDUPDATE.
    void OnClipboardChanged();

    // Invoked from the STA WndProc on WM_RENDERFORMAT. Sends DATA_REQUEST to
    // the parent, waits up to kRenderTimeout for a matching PROVIDE_DATA,
    // then calls the appropriate SetClipboardData() while the OS still has
    // the clipboard open. Runs synchronously inside the WM_RENDERFORMAT
    // dispatch but pumps messages via MsgWaitForMultipleObjects so the STA
    // thread does not deadlock against any nested OLE marshaling.
    //
    // cache_only=true is set when called from WM_RENDERALLFORMATS. In that
    // mode we MUST NOT issue a DATA_REQUEST round-trip — the OS is about
    // to take ownership away and stalling for tens of seconds per format
    // would freeze the system clipboard. Instead we only render when
    // pre-cached bytes already exist for the current announcement.
    void OnRenderFormat(unsigned int format, bool cache_only);

    // Called on the helper-side shutdown path to discard any in-flight
    // render wait. Currently only used as a defensive nudge.
    void CancelPendingRender();

    // Releases STA-bound COM resources (virtual file IDataObject) via a
    // RunSync onto the still-alive STA worker. Must be invoked BEFORE
    // sta.Stop() so the IDataObject is freed inside the apartment that
    // owns it. Safe to call multiple times.
    void ReleaseStaBoundResources(StaWorker* sta);

private:
    void HandleAnnounceDelayed(const std::vector<std::uint8_t>& sub_payload);
    void HandleProvideData(const std::vector<std::uint8_t>& sub_payload);

    // Handles FILE_CHUNK_REQUEST from the parent. Opens the file currently
    // mapped to (file_id) under file_paths_mu_, reads the requested chunk
    // off disk, and ships FILE_CHUNK_DATA back through PipeServer::SendFrame.
    // Runs on the pipe-accept thread; file I/O is independent of the STA.
    void HandleFileChunkRequest(const std::vector<std::uint8_t>& sub_payload);

    // Phase 2: handles FILE_CHUNK_DATA from the parent. The bytes are the
    // reply to a FILE_CHUNK_REQUEST our chunk_provider_ sent on behalf of
    // an outstanding IStream::Read inside a virtual-file paste. Routes the
    // payload to chunk_provider_ which unblocks the waiting STA caller.
    void HandleFileChunkData(const std::vector<std::uint8_t>& sub_payload);

    // Refresh the local file path table whenever ReadClipboard reports
    // ContentFiles. file_paths_[i] is the absolute path matching
    // FileMetadata.file_id="i" in the CLIPBOARD_CHANGED frame the parent
    // just received.
    void UpdateFilePaths(const std::vector<std::wstring>& paths);

    // Like UpdateFilePaths but for the virtual-file path (CFSTR_FILEDESCRIPTORW).
    // The IDataObject pointer is AddRef'd by the caller and we take
    // ownership — Release happens here when superseded or on Close.
    void UpdateVirtualFiles(void* data_object, std::vector<std::uint32_t> lindex_by_id);

    StaWorker*  sta_;
    PipeServer* pipe_;

    // Pending delayed-render state. Holds the announcement we last sent
    // SetClipboardData(fmt, NULL) for, plus the response slot the
    // WM_RENDERFORMAT path waits on.
    //
    // Lifecycle:
    //   HandleAnnounceDelayed  → write hash + type, reset event, drain
    //                            any stale response bytes
    //   OnRenderFormat (STA)   → send DATA_REQUEST, MsgWaitForMultipleObjects
    //                            on render_event_, copy response, SetClipboardData
    //   HandleProvideData      → validate hash, store bytes, SetEvent
    std::mutex                 render_mu_;
    std::string                pending_hash_;
    proto::ClipboardContentType pending_type_{proto::ClipboardContentType::Unspecified};
    std::vector<std::uint8_t>  pending_response_;
    // Cache of the most-recent successfully-rendered response for the
    // current announcement. Lets a second WM_RENDERFORMAT for the same
    // announcement (e.g. an app asks for CF_DIBV5 then CF_DIB, or
    // WM_RENDERALLFORMATS fans out) reuse the bytes we already fetched
    // instead of issuing another DATA_REQUEST + 120 s round-trip.
    // Cleared whenever HandleAnnounceDelayed replaces pending_hash_.
    std::vector<std::uint8_t>  cached_response_;
    bool                       cached_valid_{false};
    HANDLE                     render_event_{nullptr};  // manual-reset

    // Map of file_id → absolute Win32 path. Populated from CLIPBOARD_CHANGED
    // / GET_CLIPBOARD snapshots; consumed by HandleFileChunkRequest when
    // the parent fetches file bytes for outbound transfer. file_id is the
    // decimal index as a string (matches EncodeFileMetadata in dispatch.cpp).
    std::mutex                                    file_paths_mu_;
    std::unordered_map<std::string, std::wstring> file_paths_;

    // Virtual-file state (CFSTR_FILEDESCRIPTORW). Lives in the same mutex
    // as file_paths_ since the two are mutually exclusive — the latest
    // ReadClipboard snapshot updates one or the other, never both.
    //   virtual_data_object_ — AddRef'd IDataObject pointer, owned by us.
    //   virtual_lindex_      — file_id → lindex for IDataObject::GetData.
    //
    // This is the READ-side IDataObject (snapshot of *upstream* virtual
    // files, e.g. an Outlook attachment the user dragged onto our local
    // clipboard). Not to be confused with the WRITE-side IDataObject our
    // chunk_provider_ feeds when shen publishes remote files locally —
    // that one's lifetime is owned entirely by OLE after OleSetClipboard.
    void*                                          virtual_data_object_{nullptr};
    std::unordered_map<std::string, std::uint32_t> virtual_lindex_;

    // Phase 2 virtual-file paste backend. Alive for the life of the
    // dispatcher; outbound IDataObjects we hand to OleSetClipboard call
    // into this provider when Explorer pulls IStream chunks.
    std::unique_ptr<DispatcherChunkProvider> chunk_provider_;

    // Outbound IDataObject currently on the OS clipboard via OleSetClipboard.
    // Our refcount=1 retained reference; the OS has its own. Released via
    // RunSync on the STA worker when (a) a new ANNOUNCE_DELAYED supersedes
    // (any type), (b) a foreign clipboard owner takes over, or (c) the
    // helper shuts down (see ReleaseStaBoundResources).
    //
    // Lives in the same mutex as file_paths_ — read/write only on the STA
    // worker for OleSetClipboard calls; the pointer itself is touched
    // exclusively from the STA thread so the existing mutex coverage is
    // belt-and-braces.
    IDataObject*               outbound_data_object_{nullptr};
};

}  // namespace leviathan::clipboard_helper
