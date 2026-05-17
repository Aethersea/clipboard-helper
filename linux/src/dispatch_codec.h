#pragma once
//
// dispatch_codec — protobuf encode/decode for the HelperMessage schema.
//
// Carved out of dispatch.cpp so the wire-format surface is independently
// unit-testable without pulling in Qt or any clipboard backend. The
// Dispatcher class itself (which owns the ClipboardManager and routes
// inbound frames) stays in dispatch.cpp; only the free functions that
// move bytes in and out of the schema live here.
//
// Linux subset of the Windows dispatch_codec — P1 covers TEXT (plus the
// generic envelope plumbing). IMAGE / FILES / FILE_CHUNK encoders/parsers
// will land alongside the Wayland backend in the next PR.
//
// Field numbers and enums come from helper_proto.h.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helper_proto.h"

namespace leviathan::clipboard_helper::dispatch_codec {

// Clock injection for timestamps. Tests pass a deterministic stub;
// production passes NowUnixMillis.
using ClockFn = std::uint64_t (*)();

// Production clock — system_clock since Unix epoch, in milliseconds.
std::uint64_t NowUnixMillis();

// ─── Encoders ────────────────────────────────────────────────────────────

// READY frame sent immediately after a parent connects so it can
// confirm the helper process is alive before sending requests.
std::vector<std::uint8_t> EncodeReady(ClockFn clock = &NowUnixMillis);

// ERROR frame — wraps a free-form human-readable message. Used when
// the helper rejects an inbound frame (unknown message type, parse
// failure, oversized payload) or hits an unrecoverable backend error.
std::vector<std::uint8_t> EncodeError(std::string_view message,
                                      ClockFn          clock = &NowUnixMillis);

// CLIPBOARD_CHANGED(text): pushed when the local clipboard changes
// because of external user activity. `content_hash` is the SHA-256
// hex of payload (the parent uses it for dedup against its own
// outbound state).
std::vector<std::uint8_t> EncodeClipboardChangedText(std::string_view utf8_payload,
                                                     std::string_view content_hash,
                                                     ClockFn          clock = &NowUnixMillis);

// DATA_REQUEST: fired when the OS wants to render the bytes of a
// delayed-rendered slot we previously announced. Parent answers with
// PROVIDE_DATA whose content_hash MUST match what we sent here.
std::vector<std::uint8_t> EncodeDataRequest(std::string_view             content_hash,
                                            proto::ClipboardContentType content_type,
                                            ClockFn                      clock = &NowUnixMillis);

// ─── Parsed structs ─────────────────────────────────────────────────────
//
// Each Parse* function fills its corresponding Parsed* POD. Pointer
// fields reference into the caller-owned inbound buffer; the buffer
// must outlive the Parsed* struct.

struct ParsedHelperMessage {
    proto::HelperMessageType type{proto::HelperMessageType::Unspecified};
    const std::uint8_t*      clipboard_data_ptr{nullptr};
    std::size_t              clipboard_data_len{0};
    const std::uint8_t*      announcement_ptr{nullptr};
    std::size_t              announcement_len{0};
    const std::uint8_t*      provide_data_ptr{nullptr};
    std::size_t              provide_data_len{0};
    const std::uint8_t*      file_chunk_request_ptr{nullptr};
    std::size_t              file_chunk_request_len{0};
    const std::uint8_t*      file_chunk_data_ptr{nullptr};
    std::size_t              file_chunk_data_len{0};
};

struct ParsedClipboardData {
    proto::ClipboardContentType content_type{proto::ClipboardContentType::Unspecified};
    const std::uint8_t*         payload_ptr{nullptr};
    std::size_t                 payload_len{0};
    std::string                 content_hash;
    std::string                 transfer_id;
};

struct ParsedAnnouncement {
    proto::ClipboardContentType content_type{proto::ClipboardContentType::Unspecified};
    std::string                 content_hash;
    std::string                 transfer_id;
};

struct ParsedProvideData {
    std::string                 content_hash;
    std::vector<std::uint8_t>   data;
};

// ─── Decoders ────────────────────────────────────────────────────────────

// Parse a HelperMessage envelope. Returns false on malformed wire
// data; partial / unknown fields are tolerated (proto3 forward-compat).
bool ParseHelperMessage(const std::vector<std::uint8_t>& in,
                        ParsedHelperMessage&             out);

bool ParseClipboardData(const std::uint8_t*  data,
                        std::size_t          len,
                        ParsedClipboardData& out);

bool ParseAnnouncement(const std::uint8_t* data,
                       std::size_t         len,
                       ParsedAnnouncement& out);

bool ParseProvideData(const std::uint8_t* data,
                      std::size_t         len,
                      ParsedProvideData&  out);

}  // namespace leviathan::clipboard_helper::dispatch_codec
