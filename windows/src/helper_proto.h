#pragma once
//
// helper_proto — typed constants for the `HelperMessage` schema from
// `Proto/clipboard_helper.proto`.
//
// These are intentionally maintained by hand instead of generated to keep
// the helper independent of protobuf-runtime tooling. If the schema grows
// new fields, mirror them here.

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

}  // namespace leviathan::clipboard_helper::proto
