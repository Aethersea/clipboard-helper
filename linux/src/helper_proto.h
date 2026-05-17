#pragma once
//
// helper_proto — typed constants for the `HelperMessage` schema from
// `Proto/clipboard_helper.proto`.
//
// These are intentionally maintained by hand instead of generated to keep
// the helper independent of protobuf-runtime tooling. If the schema grows
// new fields, mirror them here and in windows/src/helper_proto.h and
// macos/Sources/ClipboardHelper/Generated/.
//
// Copied verbatim from windows/src/helper_proto.h — keep in sync.

#include <cstdint>

namespace leviathan::clipboard_helper::proto {

// Wire values for HelperMessageType (see Proto/clipboard_helper.proto).
enum class HelperMessageType : std::int32_t {
    Unspecified            = 0,

    // Parent → Helper
    SetClipboard           = 1,
    AnnounceDelayed        = 2,
    ProvideData            = 3,
    GetClipboard           = 4,
    Shutdown               = 5,

    // Helper → Parent
    ClipboardChanged       = 10,
    DataRequest            = 11,
    ClipboardContent       = 12,
    Error                  = 13,
    Ready                  = 14,

    // Bidirectional file chunk transfer
    FileChunkRequest       = 15,
    FileChunkData          = 16,

    // Parent → Helper: progress reporting
    FileTransferProgress   = 17,
};

// Wire values for ClipboardContentType (Proto/clipboard.proto).
enum class ClipboardContentType : std::int32_t {
    Unspecified = 0,
    Text        = 1,
    Image       = 2,
    Files       = 3,
};

// HelperMessage field numbers
constexpr std::uint32_t kFieldType                  = 1;
constexpr std::uint32_t kFieldClipboardData         = 2;
constexpr std::uint32_t kFieldAnnouncement          = 3;
constexpr std::uint32_t kFieldProvideData           = 4;
constexpr std::uint32_t kFieldDataRequest           = 5;
constexpr std::uint32_t kFieldErrorMessage          = 6;
constexpr std::uint32_t kFieldTimestamp             = 7;
constexpr std::uint32_t kFieldFileChunkRequest      = 8;
constexpr std::uint32_t kFieldFileChunkData         = 9;
constexpr std::uint32_t kFieldFileTransferProgress  = 10;

// ClipboardData field numbers (from Proto/clipboard.proto).
constexpr std::uint32_t kCDFieldContentType  = 1;
constexpr std::uint32_t kCDFieldPayload      = 2;
constexpr std::uint32_t kCDFieldContentHash  = 3;
constexpr std::uint32_t kCDFieldFiles        = 4;
constexpr std::uint32_t kCDFieldTransferId   = 5;

// ClipboardAnnouncement field numbers (Proto/clipboard.proto).
constexpr std::uint32_t kAnnFieldContentType = 1;
constexpr std::uint32_t kAnnFieldContentHash = 2;
constexpr std::uint32_t kAnnFieldFiles       = 6;
constexpr std::uint32_t kAnnFieldTransferId  = 7;

// ClipboardDataRequest field numbers (outbound when OS triggers a paste).
constexpr std::uint32_t kReqFieldContentHash = 1;
constexpr std::uint32_t kReqFieldContentType = 2;

// HelperProvideData field numbers (inbound, replies to a DATA_REQUEST).
constexpr std::uint32_t kPDFieldContentHash  = 1;
constexpr std::uint32_t kPDFieldData         = 2;

// FileMetadata field numbers (Proto/clipboard.proto). Referenced by future
// FILES-path encoders/decoders; included for completeness so the constants
// don't drift between platforms.
constexpr std::uint32_t kFMFieldFileId        = 1;
constexpr std::uint32_t kFMFieldFilename      = 2;
constexpr std::uint32_t kFMFieldRelativePath  = 3;
constexpr std::uint32_t kFMFieldFileSize      = 4;
constexpr std::uint32_t kFMFieldMimeType      = 5;
constexpr std::uint32_t kFMFieldChecksum      = 6;
constexpr std::uint32_t kFMFieldIsDirectory   = 7;

}  // namespace leviathan::clipboard_helper::proto
