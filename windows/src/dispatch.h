#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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
    HANDLE                     render_event_{nullptr};  // manual-reset
};

}  // namespace leviathan::clipboard_helper
