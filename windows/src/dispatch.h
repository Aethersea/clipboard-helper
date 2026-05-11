#pragma once

#include <cstdint>
#include <vector>

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

private:
    StaWorker*  sta_;
    PipeServer* pipe_;
};

}  // namespace leviathan::clipboard_helper
