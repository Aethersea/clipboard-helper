#pragma once
//
// dispatch_codec — protobuf encode/decode for the HelperMessage schema.
//
// Carved out of dispatch.cpp so the wire-format surface is independently
// unit-testable without pulling in OLE, the STA worker, or any other
// runtime dependency. The Dispatcher class itself (which owns the
// IDataObject lifetime and drives WM_RENDERFORMAT) stays in dispatch.cpp;
// only the free functions that move bytes in and out of the schema live
// here.
//
// Field numbers and enums come from helper_proto.h, which mirrors the
// .proto source of truth.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clipboard_ops.h"      // ClipboardSnapshot (encode input)
#include "helper_proto.h"
#include "virtual_file_spec.h"  // VirtualFileSpec (Files-announce parse output)

namespace leviathan::clipboard_helper {

namespace dispatch_codec {

// FileMetadata field numbers (from Proto/clipboard.proto). Exposed so
// the test surface can reference them by name when constructing inputs.
constexpr std::uint32_t kFMFieldFileId       = 1;
constexpr std::uint32_t kFMFieldFilename     = 2;
constexpr std::uint32_t kFMFieldRelativePath = 3;
constexpr std::uint32_t kFMFieldFileSize     = 4;
constexpr std::uint32_t kFMFieldIsDirectory  = 7;

// Source of the clock value embedded in HelperMessage.timestamp. Tests
// pass a deterministic millisecond count; production uses NowUnixMillis
// which reads system_clock.
using ClockFn = std::uint64_t (*)();

// Production clock — system_clock since Unix epoch, millis. Exposed so
// callers don't need to duplicate the chrono incantation; tests can
// substitute a deterministic stub.
std::uint64_t NowUnixMillis();

// ─── Encoders ─────────────────────────────────────────────────────────────
//
// Every encoder follows the same shape: take POD args, return a
// HelperMessage envelope ready for PipeServer::SendFrame (no length
// prefix — the pipe layer adds that). The `clock` parameter lets tests
// pin the timestamp; production callers pass NowUnixMillis.

std::vector<std::uint8_t> EncodeReady(ClockFn clock = &NowUnixMillis);

std::vector<std::uint8_t> EncodeError(std::string_view message,
                                      ClockFn clock = &NowUnixMillis);

// Encode a single FileMetadata sub-message for a CF_HDROP-derived path.
// Calls GetFileAttributesExW(full_path) for the size field; non-existent
// paths report size 0.
std::vector<std::uint8_t> EncodeFileMetadata(std::uint32_t index,
                                             const std::wstring& full_path);

// Encode a FileMetadata sub-message for a CFSTR_FILEDESCRIPTORW-derived
// virtual file. relative_path stays empty (no on-disk path) and size
// comes from the snapshot directly.
std::vector<std::uint8_t> EncodeVirtualFileMetadata(std::uint32_t index,
                                                    const VirtualFileEntry& entry);

// Encode a ClipboardData sub-message from a decoded snapshot.
std::vector<std::uint8_t> EncodeClipboardData(const ClipboardSnapshot& snap);

// Wrap a ClipboardData inner buffer into a HelperMessage envelope.
std::vector<std::uint8_t> EncodeClipboardEnvelope(proto::HelperMessageType outer,
                                                  const std::vector<std::uint8_t>& clipboard_data,
                                                  ClockFn clock = &NowUnixMillis);

// Encode an outbound DATA_REQUEST when the OS asks us to materialize a
// delayed format.
std::vector<std::uint8_t> EncodeDataRequest(std::string_view content_hash,
                                            proto::ClipboardContentType content_type,
                                            ClockFn clock = &NowUnixMillis);

// Encode an outbound FILE_CHUNK_REQUEST for an IStream::Read on the
// virtual-file paste path.
std::vector<std::uint8_t> EncodeFileChunkRequestOutbound(const std::string& transfer_id,
                                                         const std::string& file_id,
                                                         std::uint64_t      offset,
                                                         std::uint32_t      size,
                                                         ClockFn clock = &NowUnixMillis);

// Encode an outbound FILE_CHUNK_DATA envelope (helper-side reply to a
// FILE_CHUNK_REQUEST from the parent).
std::vector<std::uint8_t> EncodeFileChunkData(const std::string& transfer_id,
                                              const std::string& file_id,
                                              std::uint64_t       offset,
                                              const std::uint8_t* data,
                                              std::size_t         data_len,
                                              bool                is_last,
                                              std::string_view    error_msg,
                                              ClockFn clock = &NowUnixMillis);

// ─── Parsed structs ───────────────────────────────────────────────────────
//
// Every Parse* function fills a Parsed* POD. Pointer fields point into
// the inbound byte buffer the caller owns — callers must keep the buffer
// alive for the lifetime of the Parsed* struct's pointer fields.

struct ParsedHelperMessage {
    proto::HelperMessageType type{proto::HelperMessageType::Unspecified};
    const std::uint8_t* clipboard_data_ptr{nullptr};
    std::size_t         clipboard_data_len{0};
    const std::uint8_t* announcement_ptr{nullptr};
    std::size_t         announcement_len{0};
    const std::uint8_t* provide_data_ptr{nullptr};
    std::size_t         provide_data_len{0};
    const std::uint8_t* file_chunk_request_ptr{nullptr};
    std::size_t         file_chunk_request_len{0};
    const std::uint8_t* file_chunk_data_ptr{nullptr};
    std::size_t         file_chunk_data_len{0};
};

struct ParsedClipboardData {
    proto::ClipboardContentType content_type{proto::ClipboardContentType::Unspecified};
    const std::uint8_t* payload_ptr{nullptr};
    std::size_t         payload_len{0};
    std::vector<std::pair<const std::uint8_t*, std::size_t>> files;
};

struct ParsedAnnouncement {
    proto::ClipboardContentType content_type{proto::ClipboardContentType::Unspecified};
    std::string                 content_hash;
    std::vector<std::pair<const std::uint8_t*, std::size_t>> files;
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
    std::uint32_t size{0};
};

struct ParsedFileChunkData {
    std::string                 transfer_id;
    std::string                 file_id;
    std::uint64_t               offset{0};
    std::vector<std::uint8_t>   data;
    bool                        is_last{false};
    std::string                 error;
};

// ─── Decoders ─────────────────────────────────────────────────────────────

bool ParseHelperMessage(const std::vector<std::uint8_t>& in, ParsedHelperMessage& out);

bool ParseClipboardData(const std::uint8_t* data, std::size_t len, ParsedClipboardData& out);

bool ParseAnnouncement(const std::uint8_t* data, std::size_t len, ParsedAnnouncement& out);

// Extract the subset of FileMetadata fields needed for the virtual-file
// IDataObject (file_id, filename, file_size, is_directory). Returns false
// when the entry is malformed; callers should skip it and continue.
bool ParseFileMetadataForVirtualFile(const std::uint8_t* data, std::size_t len,
                                     VirtualFileSpec& out);

bool ParseProvideData(const std::uint8_t* data, std::size_t len, ParsedProvideData& out);

bool ParseFileChunkRequest(const std::uint8_t* data, std::size_t len, ParsedFileChunkRequest& out);

bool ParseFileChunkData(const std::uint8_t* data, std::size_t len, ParsedFileChunkData& out);

// Extract FileMetadata.relative_path (field 3, string) from a sub-message
// payload. Used by SET_CLIPBOARD content_type=Files to recover the
// absolute Win32 paths the Go side encoded.
bool ExtractRelativePath(const std::uint8_t* data, std::size_t len, std::string& out);

}  // namespace dispatch_codec

}  // namespace leviathan::clipboard_helper
