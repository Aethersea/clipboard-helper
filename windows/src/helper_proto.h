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

// HelperFileChunkRequest field numbers (clipboard_helper.proto).
constexpr std::uint32_t kFCReqFieldTransferId = 1;
constexpr std::uint32_t kFCReqFieldFileId     = 2;
constexpr std::uint32_t kFCReqFieldOffset     = 3;
constexpr std::uint32_t kFCReqFieldSize       = 4;

// HelperFileChunkData field numbers (clipboard_helper.proto).
constexpr std::uint32_t kFCDataFieldTransferId = 1;
constexpr std::uint32_t kFCDataFieldFileId     = 2;
constexpr std::uint32_t kFCDataFieldOffset     = 3;
constexpr std::uint32_t kFCDataFieldData       = 4;
constexpr std::uint32_t kFCDataFieldIsLast     = 5;
constexpr std::uint32_t kFCDataFieldError      = 6;

// ClipboardAnnouncement field numbers (from Proto/clipboard.proto). Only the
// fields we actually consume on the helper side are listed.
constexpr std::uint32_t kAnnFieldContentType = 1;
constexpr std::uint32_t kAnnFieldContentHash = 2;
// `repeated FileMetadata files = 6` carries the metadata the helper needs
// to publish virtual files (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS)
// — name, size, file_id — without first downloading payload bytes.
constexpr std::uint32_t kAnnFieldFiles       = 6;
constexpr std::uint32_t kAnnFieldTransferId  = 7;

// ClipboardDataRequest field numbers (the outbound DATA_REQUEST we send when
// the OS fires WM_RENDERFORMAT and we need the parent to materialize data).
constexpr std::uint32_t kReqFieldContentHash = 1;
constexpr std::uint32_t kReqFieldContentType = 2;

// HelperProvideData field numbers (inbound PROVIDE_DATA replying to a
// DATA_REQUEST we previously sent).
constexpr std::uint32_t kPDFieldContentHash  = 1;
constexpr std::uint32_t kPDFieldData         = 2;

}  // namespace leviathan::clipboard_helper::proto
