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
// Linux subset of the Windows dispatch_codec: TEXT plus the generic
// envelope plumbing, CLIPBOARD_CHANGED(FILES) for local file copies, and
// the FILE_CHUNK_REQUEST / FILE_CHUNK_DATA pair used to serve those
// files to the parent. (SET_CLIPBOARD with FILES is not supported on
// Linux, so there is no inbound FileMetadata parser.)
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

// One entry of a CLIPBOARD_CHANGED(FILES) frame — the FileMetadata
// fields the Linux helper fills (same subset as the macOS helper's
// clipboardDataFromFileURLs). mime_type / checksum_sha256 stay empty.
struct LocalFileMetadata {
    std::string   file_id;        // decimal index "0", "1", …
    std::string   filename;       // basename
    std::string   relative_path;  // absolute local path
    std::uint64_t file_size{0};
    bool          is_directory{false};
};

// CLIPBOARD_CHANGED(files): pushed when the user copies files in a file
// manager. payload stays empty; the parent pulls the bytes later via
// FILE_CHUNK_REQUEST keyed by file_id.
std::vector<std::uint8_t> EncodeClipboardChangedFiles(const std::vector<LocalFileMetadata>& files,
                                                      std::string_view content_hash,
                                                      ClockFn          clock = &NowUnixMillis);

// FILE_CHUNK_DATA: the helper's reply to a FILE_CHUNK_REQUEST. On error
// pass empty data, is_last=true and a non-empty `error_msg`.
std::vector<std::uint8_t> EncodeFileChunkData(std::string_view    transfer_id,
                                              std::string_view    file_id,
                                              std::uint64_t       offset,
                                              const std::uint8_t* data,
                                              std::size_t         data_len,
                                              bool                is_last,
                                              std::string_view    error_msg,
                                              ClockFn             clock = &NowUnixMillis);

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

struct ParsedFileChunkRequest {
    std::string   transfer_id;
    std::string   file_id;
    std::uint64_t offset{0};
    std::uint32_t size{0};  // 0 = use the 262144-byte default
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

bool ParseFileChunkRequest(const std::uint8_t*     data,
                           std::size_t             len,
                           ParsedFileChunkRequest& out);

}  // namespace leviathan::clipboard_helper::dispatch_codec
