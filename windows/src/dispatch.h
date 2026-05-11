#pragma once

#include <cstdint>
#include <vector>

namespace leviathan {

// Forward declaration of the generated message, kept out of the header so the
// public surface stays free of protobuf-generated includes (the pipe_server
// layer only sees opaque byte buffers).
class HelperMessage;

namespace clipboard_helper {

// MakeReadyFrame builds an outbound HELPER_MESSAGE_TYPE_READY frame so the
// pipe server can ship a handshake message immediately after a client
// connects. Returns the marshaled length-prefix payload (already serialized;
// the pipe_server adds the uint32 LE length prefix on top).
std::vector<std::uint8_t> MakeReadyFrame();

// HandleHelperMessage parses an incoming length-prefix payload as a
// HelperMessage and dispatches by type. In Phase 2a most handlers are stubs
// that log the receipt and return an empty reply; later phases will fill them
// in with actual clipboard/OLE work.
//
// Returns the bytes to write back, or an empty vector if no reply is
// warranted for this message.
std::vector<std::uint8_t> HandleHelperMessage(const std::vector<std::uint8_t>& request);

}  // namespace clipboard_helper
}  // namespace leviathan
