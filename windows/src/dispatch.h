#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "helper_proto.h"

namespace leviathan::clipboard_helper {

class StaWorker;
class PipeServer;

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
    void OnRenderFormat(unsigned int format);

    // Called on the helper-side shutdown path to discard any in-flight
    // render wait. Currently only used as a defensive nudge.
    void CancelPendingRender();

private:
    void HandleAnnounceDelayed(const std::vector<std::uint8_t>& sub_payload);
    void HandleProvideData(const std::vector<std::uint8_t>& sub_payload);

    // Handles FILE_CHUNK_REQUEST from the parent. Opens the file currently
    // mapped to (file_id) under file_paths_mu_, reads the requested chunk
    // off disk, and ships FILE_CHUNK_DATA back through PipeServer::SendFrame.
    // Runs on the pipe-accept thread; file I/O is independent of the STA.
    void HandleFileChunkRequest(const std::vector<std::uint8_t>& sub_payload);

    // Refresh the local file path table whenever ReadClipboard reports
    // ContentFiles. file_paths_[i] is the absolute path matching
    // FileMetadata.file_id="i" in the CLIPBOARD_CHANGED frame the parent
    // just received.
    void UpdateFilePaths(const std::vector<std::wstring>& paths);

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
};

}  // namespace leviathan::clipboard_helper
