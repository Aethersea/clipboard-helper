#include "dispatch.h"

#include <ole2.h>          // OleSetClipboard (Phase 2 virtual-file paste path)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <future>
#include <string>
#include <utility>

#include "clipboard_ops.h"
#include "helper_proto.h"
#include "log.h"
#include "pipe_server.h"
#include "proto_wire.h"
#include "sta_worker.h"
#include "virtual_file_provider.h"

namespace leviathan::clipboard_helper {

using proto::ClipboardContentType;
using proto::HelperMessageType;
using proto_wire::Reader;
using proto_wire::WireType;
using proto_wire::Writer;

namespace {

std::uint64_t NowUnixMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ─── Encoders ─────────────────────────────────────────────────────────────

std::vector<std::uint8_t> EncodeReady() {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Ready));
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

std::vector<std::uint8_t> EncodeError(std::string_view message) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Error));
    if (!message.empty()) {
        w.WriteStringField(proto::kFieldErrorMessage, message);
    }
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// FileMetadata field numbers (from Proto/clipboard.proto). Encoded as the
// repeated `files` sub-message of ClipboardData / ClipboardAnnouncement.
constexpr std::uint32_t kFMFieldFileId       = 1;
constexpr std::uint32_t kFMFieldFilename     = 2;
constexpr std::uint32_t kFMFieldRelativePath = 3;
constexpr std::uint32_t kFMFieldFileSize     = 4;
constexpr std::uint32_t kFMFieldIsDirectory  = 7;

// Encode a single FileMetadata sub-message for a CF_HDROP-derived path.
// We populate file_id (the index), filename (basename), relative_path
// (full path, so the parent can later issue FILE_CHUNK_REQUEST against it
// in Phase 4b), and file_size when we can stat() the file.
std::vector<std::uint8_t> EncodeFileMetadata(std::uint32_t index, const std::wstring& full_path) {
    const std::string id_str = std::to_string(index);
    const std::string utf8_full = WideToUtf8(full_path);
    // basename: keep everything after the last '\\' or '/'.
    std::wstring basename = full_path;
    const auto slash = full_path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        basename = full_path.substr(slash + 1);
    }
    const std::string utf8_base = WideToUtf8(basename);

    std::uint64_t file_size = 0;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (::GetFileAttributesExW(full_path.c_str(), GetFileExInfoStandard, &info)) {
        ULARGE_INTEGER sz{};
        sz.LowPart  = info.nFileSizeLow;
        sz.HighPart = info.nFileSizeHigh;
        file_size = sz.QuadPart;
    }

    Writer w;
    w.WriteStringField(kFMFieldFileId, id_str);
    w.WriteStringField(kFMFieldFilename, utf8_base);
    w.WriteStringField(kFMFieldRelativePath, utf8_full);
    w.WriteUint64Field(kFMFieldFileSize, file_size);
    return w.take();
}

// Encode a single FileMetadata sub-message for a CFSTR_FILEDESCRIPTORW-
// derived virtual file. We don't have an on-disk path, so relative_path
// stays empty; the parent must use the file_id when issuing
// FILE_CHUNK_REQUEST.
std::vector<std::uint8_t> EncodeVirtualFileMetadata(std::uint32_t index,
                                                    const VirtualFileEntry& entry) {
    const std::string id_str   = std::to_string(index);
    const std::string utf8_name = WideToUtf8(entry.name);

    Writer w;
    w.WriteStringField(kFMFieldFileId, id_str);
    w.WriteStringField(kFMFieldFilename, utf8_name);
    // relative_path intentionally empty for virtual files; parent should
    // not try to dereference it as a local FS path.
    w.WriteUint64Field(kFMFieldFileSize, entry.size);
    return w.take();
}

// Encode a ClipboardData submessage given an already-decoded snapshot.
std::vector<std::uint8_t> EncodeClipboardData(const ClipboardSnapshot& snap) {
    Writer w;
    w.WriteEnumField(proto::kCDFieldContentType, static_cast<std::int32_t>(snap.content_type));
    switch (snap.content_type) {
        case ClipboardContentType::Text: {
            const std::string utf8 = WideToUtf8(snap.text);
            w.WriteBytesField(proto::kCDFieldPayload,
                              reinterpret_cast<const std::uint8_t*>(utf8.data()),
                              utf8.size());
            break;
        }
        case ClipboardContentType::Image:
            w.WriteBytesField(proto::kCDFieldPayload,
                              snap.image_png.data(), snap.image_png.size());
            break;
        case ClipboardContentType::Files:
            // Repeated FileMetadata via field tag = (kCDFieldFiles<<3) | 2.
            // Physical (CF_HDROP) and virtual (CFSTR_FILEDESCRIPTORW)
            // entries share the same wire field; only one of file_paths /
            // virtual_files is populated per snapshot.
            for (std::size_t i = 0; i < snap.file_paths.size(); ++i) {
                w.WriteSubMessageField(proto::kCDFieldFiles,
                                       EncodeFileMetadata(static_cast<std::uint32_t>(i),
                                                          snap.file_paths[i]));
            }
            for (std::size_t i = 0; i < snap.virtual_files.size(); ++i) {
                w.WriteSubMessageField(proto::kCDFieldFiles,
                                       EncodeVirtualFileMetadata(static_cast<std::uint32_t>(i),
                                                                  snap.virtual_files[i]));
            }
            break;
        case ClipboardContentType::Unspecified:
            break;
    }
    return w.take();
}

// Wrap a ClipboardData inner buffer into a HelperMessage with the requested
// outer type (CLIPBOARD_CHANGED or CLIPBOARD_CONTENT).
std::vector<std::uint8_t> EncodeClipboardEnvelope(HelperMessageType outer,
                                                  const std::vector<std::uint8_t>& clipboard_data) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(outer));
    w.WriteSubMessageField(proto::kFieldClipboardData, clipboard_data);
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// Encode an outbound DATA_REQUEST when the OS asks us to materialize a
// delayed format. The sub-message is ClipboardDataRequest { content_hash,
// content_type }.
std::vector<std::uint8_t> EncodeDataRequest(std::string_view content_hash,
                                            ClipboardContentType content_type) {
    Writer inner;
    inner.WriteStringField(proto::kReqFieldContentHash, content_hash);
    inner.WriteEnumField(proto::kReqFieldContentType, static_cast<std::int32_t>(content_type));

    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::DataRequest));
    w.WriteSubMessageField(proto::kFieldDataRequest, inner.bytes());
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// Phase 2 outbound FILE_CHUNK_REQUEST. Symmetric to the inbound encoder
// EncodeFileChunkData below but for the request side of the bidirectional
// FILE_CHUNK_* pair — used by DispatcherChunkProvider when an
// IStream::Read needs bytes from the parent.
std::vector<std::uint8_t> EncodeFileChunkRequestOutbound(const std::string& transfer_id,
                                                         const std::string& file_id,
                                                         std::uint64_t      offset,
                                                         std::uint32_t      size) {
    Writer inner;
    inner.WriteStringField(proto::kFCReqFieldTransferId, transfer_id);
    inner.WriteStringField(proto::kFCReqFieldFileId, file_id);
    inner.WriteUint64Field(proto::kFCReqFieldOffset, offset);
    inner.WriteUint64Field(proto::kFCReqFieldSize, size);

    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::FileChunkRequest));
    w.WriteSubMessageField(proto::kFieldFileChunkRequest, inner.bytes());
    w.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return w.take();
}

// ─── Decoders ─────────────────────────────────────────────────────────────

struct ParsedHelperMessage {
    HelperMessageType type{HelperMessageType::Unspecified};
    // Sub-message slots — pointers into the inbound buffer. Set only when
    // the corresponding field appears on the wire.
    const std::uint8_t* clipboard_data_ptr{nullptr};
    std::size_t         clipboard_data_len{0};
    const std::uint8_t* announcement_ptr{nullptr};
    std::size_t         announcement_len{0};
    const std::uint8_t* provide_data_ptr{nullptr};
    std::size_t         provide_data_len{0};
    const std::uint8_t* file_chunk_request_ptr{nullptr};
    std::size_t         file_chunk_request_len{0};
    // Phase 2: FILE_CHUNK_DATA inbound — the parent's reply to a
    // FILE_CHUNK_REQUEST we sent on behalf of an IStream::Read inside the
    // virtual-file paste path.
    const std::uint8_t* file_chunk_data_ptr{nullptr};
    std::size_t         file_chunk_data_len{0};
};

bool ParseHelperMessage(const std::vector<std::uint8_t>& in, ParsedHelperMessage& out) {
    Reader r(in);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kFieldType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.type = static_cast<HelperMessageType>(v);
            continue;
        }
        if (field == proto::kFieldClipboardData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.clipboard_data_ptr, out.clipboard_data_len)) {
                return false;
            }
            continue;
        }
        if (field == proto::kFieldAnnouncement && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.announcement_ptr, out.announcement_len)) {
                return false;
            }
            continue;
        }
        if (field == proto::kFieldProvideData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.provide_data_ptr, out.provide_data_len)) {
                return false;
            }
            continue;
        }
        if (field == proto::kFieldFileChunkRequest && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.file_chunk_request_ptr, out.file_chunk_request_len)) {
                return false;
            }
            continue;
        }
        if (field == proto::kFieldFileChunkData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.file_chunk_data_ptr, out.file_chunk_data_len)) {
                return false;
            }
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

// Parse just enough of a ClipboardData sub-message to extract the content
// type, the payload bytes (text + image), and any embedded FileMetadata
// sub-messages (used by SET_CLIPBOARD when content_type=Files; the Go
// side encodes paths via the repeated `files` field, not `payload`).
struct ParsedClipboardData {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    const std::uint8_t*  payload_ptr{nullptr};
    std::size_t          payload_len{0};
    // repeated FileMetadata files = 4: we collect each sub-message's
    // (pointer, length) and let the SET path decode them on demand.
    std::vector<std::pair<const std::uint8_t*, std::size_t>> files;
};

struct ParsedAnnouncement {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    std::string          content_hash;
    // Phase 2: per-file metadata sub-messages (CFSTR_FILEDESCRIPTORW
    // content). Each pair is (FileMetadata sub-message bytes, length).
    std::vector<std::pair<const std::uint8_t*, std::size_t>> files;
    // Announcement-scoped identifier the parent uses on the WebRTC side to
    // route this file set's chunk fetches. Helper threads it back through
    // every FILE_CHUNK_REQUEST so shen can resolve (transfer_id, file_id)
    // → upstream WebRTC stream without first downloading the entire set.
    std::string          transfer_id;
};

struct ParsedProvideData {
    std::string                 content_hash;
    std::vector<std::uint8_t>   data;
};

bool ParseAnnouncement(const std::uint8_t* data, std::size_t len, ParsedAnnouncement& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kAnnFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(v);
            continue;
        }
        if (field == proto::kAnnFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == proto::kAnnFieldFiles && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.files.emplace_back(p, n);
            continue;
        }
        if (field == proto::kAnnFieldTransferId && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.transfer_id.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

// Extract the subset of FileMetadata fields the virtual-file IDataObject
// needs (file_id, filename, file_size, is_directory). Returns false when
// the entry is malformed; callers should skip it and continue.
bool ParseFileMetadataForVirtualFile(const std::uint8_t* data, std::size_t len,
                                     VirtualFileSpec& out) {
    Reader r(data, len);
    std::string file_id;
    std::string filename;
    std::string relative_path;
    std::uint64_t file_size = 0;
    bool is_directory = false;
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == kFMFieldFileId && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr; std::size_t n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            file_id.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == kFMFieldFilename && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr; std::size_t n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            filename.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == kFMFieldRelativePath && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr; std::size_t n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            relative_path.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == kFMFieldFileSize && wire == WireType::Varint) {
            std::uint64_t v = 0; if (!r.ReadVarint(v)) return false;
            file_size = v;
            continue;
        }
        if (field == kFMFieldIsDirectory && wire == WireType::Varint) {
            std::uint64_t v = 0; if (!r.ReadVarint(v)) return false;
            is_directory = (v != 0);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    if (file_id.empty()) return false;
    // Prefer relative_path (it carries the dir-tree structure the shell
    // reconstructs at the destination); fall back to filename when the
    // upstream didn't set one.
    const std::string& display = !relative_path.empty() ? relative_path : filename;
    if (display.empty()) return false;
    out.name         = Utf8ToWide(display);
    out.size         = file_size;
    out.file_id      = std::move(file_id);
    out.is_directory = is_directory;
    return true;
}

bool ParseProvideData(const std::uint8_t* data, std::size_t len, ParsedProvideData& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kPDFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
            continue;
        }
        if (field == proto::kPDFieldData && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.data.assign(p, p + n);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

bool ParseClipboardData(const std::uint8_t* data, std::size_t len, ParsedClipboardData& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kCDFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(v);
            continue;
        }
        if (field == proto::kCDFieldPayload && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.payload_ptr, out.payload_len)) return false;
            continue;
        }
        if (field == proto::kCDFieldFiles && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.files.emplace_back(p, n);
            continue;
        }
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

// Phase 2 inbound FILE_CHUNK_DATA payload: the parent's reply to a
// FILE_CHUNK_REQUEST we sent on behalf of an IStream::Read inside a
// virtual-file paste.
struct ParsedFileChunkData {
    std::string                 transfer_id;
    std::string                 file_id;
    std::uint64_t               offset{0};
    std::vector<std::uint8_t>   data;
    bool                        is_last{false};
    std::string                 error;
};

bool ParseFileChunkData(const std::uint8_t* data, std::size_t len, ParsedFileChunkData& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        switch (field) {
            case proto::kFCDataFieldTransferId:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p = nullptr; std::size_t n = 0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.transfer_id.assign(reinterpret_cast<const char*>(p), n); }
                continue;
            case proto::kFCDataFieldFileId:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p = nullptr; std::size_t n = 0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.file_id.assign(reinterpret_cast<const char*>(p), n); }
                continue;
            case proto::kFCDataFieldOffset:
                if (wire != WireType::Varint) { if (!r.SkipField(wire)) return false; continue; }
                { std::uint64_t v = 0; if (!r.ReadVarint(v)) return false; out.offset = v; }
                continue;
            case proto::kFCDataFieldData:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p = nullptr; std::size_t n = 0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.data.assign(p, p + n); }
                continue;
            case proto::kFCDataFieldIsLast:
                if (wire != WireType::Varint) { if (!r.SkipField(wire)) return false; continue; }
                { std::uint64_t v = 0; if (!r.ReadVarint(v)) return false; out.is_last = (v != 0); }
                continue;
            case proto::kFCDataFieldError:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p = nullptr; std::size_t n = 0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.error.assign(reinterpret_cast<const char*>(p), n); }
                continue;
            default:
                if (!r.SkipField(wire)) return false;
                continue;
        }
    }
    return true;
}

// Extract FileMetadata.relative_path (field 3, string) from a sub-message
// payload. Used by SET_CLIPBOARD content_type=Files to recover the
// absolute Win32 paths the Go side encoded.
bool ExtractRelativePath(const std::uint8_t* data, std::size_t len, std::string& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        // FileMetadata field 3 = relative_path (string).
        if (field == 3 && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.assign(reinterpret_cast<const char*>(p), n);
            return true;
        }
        if (!r.SkipField(wire)) return false;
    }
    return false;
}

}  // namespace

// ─── Dispatcher ───────────────────────────────────────────────────────────

namespace {

// We mirror the macOS helper's 120s upper bound on PROVIDE_DATA waiters so
// large remote-rendered images still arrive before the OS gives up on us.
// The Go client uses a 60s budget — the asymmetry (helper waits longer than
// the client) is intentional and prevents the stale-semaphore class of bug
// the macOS helper hit in April 2026 (see vault notes via NotebookLM).
constexpr DWORD kRenderTimeoutMs = 120 * 1000;

// Phase 2: per-chunk timeout for the FILE_CHUNK_REQUEST round-trip a
// VirtualFileStream::Read does on the STA thread. Tighter than the legacy
// 120 s render budget because Explorer's copy dialog already shows progress
// — long stalls now surface as visible "slow" instead of a frozen shell.
// Caps at 30 s so a single WebRTC packet loss event doesn't pin the STA.
constexpr DWORD kChunkTimeoutMs = 30 * 1000;

}  // namespace

// ─── DispatcherChunkProvider ─────────────────────────────────────────────
//
// Lives in the named namespace so dispatch.h can forward-declare it and
// hold a std::unique_ptr<DispatcherChunkProvider>. The Dispatcher creates
// one instance per process lifetime; every virtual-file IDataObject the
// dispatcher publishes via OleSetClipboard borrows this provider via the
// ChunkProvider* abstract base.
//
// Threading:
//   * FetchChunk() runs on the STA worker thread (COM marshals the
//     IStream::Read call there). It SendFrame's a FILE_CHUNK_REQUEST and
//     waits for the parent's FILE_CHUNK_DATA reply via
//     MsgWaitForMultipleObjects, which keeps the STA pumping window
//     messages so nested COM marshaling can land.
//   * DeliverChunk() runs on the pipe-accept thread when the parent's
//     FILE_CHUNK_DATA arrives. It finds the matching PendingChunk by
//     transfer_id, stages the bytes, and SetEvent's that specific waiter.
//
// Concurrency model:
//   The STA's inner message pump inside FetchChunk WILL re-enter
//   IStream::Read for sibling streams (e.g. Explorer copying multiple
//   files in parallel — COM marshals the Read calls into the same STA,
//   and our pump dispatches them). The provider therefore must support
//   multiple in-flight requests at once. Each request gets its own auto-
//   reset event and its own PendingChunk struct; the map keyed by
//   transfer_id is the only shared state.
class DispatcherChunkProvider final : public ChunkProvider {
public:
    explicit DispatcherChunkProvider(PipeServer* pipe) : pipe_(pipe) {}
    ~DispatcherChunkProvider() override {
        // Close any leftover events. In practice ReleaseStaBoundResources
        // → CancelAll has already cleaned out the map by the time we get
        // here; this is defensive against torn shutdown paths.
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [_, queue] : pending_) {
            for (auto& slot : queue) {
                if (slot && slot->event != nullptr) {
                    ::CloseHandle(slot->event);
                    slot->event = nullptr;
                }
            }
        }
        pending_.clear();
    }

    DispatcherChunkProvider(const DispatcherChunkProvider&)            = delete;
    DispatcherChunkProvider& operator=(const DispatcherChunkProvider&) = delete;

    HRESULT FetchChunk(const std::string&         transfer_id,
                       const std::string&         file_id,
                       std::uint64_t              offset,
                       std::uint32_t              size,
                       std::vector<std::uint8_t>& out_data,
                       bool&                      out_is_last) override {
        out_data.clear();
        out_is_last = false;
        if (pipe_ == nullptr) return E_FAIL;
        if (transfer_id.empty() || file_id.empty()) return E_INVALIDARG;

        // Correlate replies by the (transfer_id, file_id, offset) tuple
        // the parent already echoes back in HelperFileChunkData. transfer_id
        // is the announcement-scoped identifier shen uses to route the
        // request upstream to leviathan; file_id+offset disambiguate
        // requests within an announcement.
        const std::string key = MakeKey(transfer_id, file_id, offset);

        auto slot = std::make_shared<PendingChunk>();
        slot->event = ::CreateEventW(nullptr, /*manualReset=*/FALSE,
                                     /*initial=*/FALSE, nullptr);
        if (slot->event == nullptr) {
            LH_LOG_ERROR("DispatcherChunkProvider: CreateEventW failed");
            return E_FAIL;
        }
        // Stage in the per-key FIFO queue BEFORE sending the frame so the
        // pipe thread can find us as soon as the reply lands. A FIFO (not
        // a single slot) lets us survive the edge case where two distinct
        // streams happen to issue overlapping Reads at the SAME
        // (transfer_id, file_id, offset) — without a queue the second
        // staging would overwrite the first's slot and strand the first
        // request until timeout. In practice the shell never does this,
        // but a queue is cheap and removes a class of timeout. No
        // process-wide "canceled" gate either — cancellation is per-slot
        // via PendingChunk::cancelled set by CancelAll on existing waiters.
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_[key].push_back(slot);
        }

        const auto frame = EncodeFileChunkRequestOutbound(transfer_id, file_id, offset, size);
        if (!pipe_->SendFrame(frame)) {
            std::lock_guard<std::mutex> lock(mu_);
            EraseFromQueueLocked(key, slot);
            ::CloseHandle(slot->event);
            return E_FAIL;
        }

        // Wait on this request's specific event. MsgWaitForMultipleObjects
        // with QS_ALLINPUT keeps the STA pumping so sibling IStream::Read
        // calls (for other files in a multi-file paste) and the helper's
        // own WM_CLIPBOARDUPDATE notifications can still dispatch.
        const DWORD start = ::GetTickCount();
        bool got_quit = false;
        for (;;) {
            const DWORD elapsed = ::GetTickCount() - start;
            if (elapsed >= kChunkTimeoutMs) {
                LH_LOG_WARN("DispatcherChunkProvider: FILE_CHUNK_REQUEST timeout");
                break;
            }
            const DWORD remaining = kChunkTimeoutMs - elapsed;
            const DWORD wait_result = ::MsgWaitForMultipleObjects(
                1, &slot->event, FALSE, remaining, QS_ALLINPUT);
            if (wait_result == WAIT_OBJECT_0) {
                break;
            }
            if (wait_result == WAIT_OBJECT_0 + 1) {
                MSG msg{};
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        ::PostQuitMessage(static_cast<int>(msg.wParam));
                        got_quit = true;
                        break;
                    }
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                if (got_quit) break;
                // After the pump, our event may have been set by a
                // re-entrant DeliverChunk OR by CancelAll; re-check
                // under the lock.
                std::lock_guard<std::mutex> lock(mu_);
                if (slot->cancelled || slot->received) break;
                continue;
            }
            // WAIT_TIMEOUT, WAIT_FAILED, etc. — bail out.
            break;
        }

        // Detach THIS specific slot from the per-key queue under the lock.
        // Removing only our entry (vs erasing the whole key) is important
        // when sibling FetchChunk calls for the same (transfer_id, file_id,
        // offset) are queued — they must remain in pending_ so the next
        // DeliverChunk can find them.
        bool received = false;
        bool canceled = false;
        std::vector<std::uint8_t> data;
        bool is_last = false;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(mu_);
            received = slot->received;
            canceled = slot->cancelled;
            data     = std::move(slot->data);
            is_last  = slot->is_last;
            error    = std::move(slot->error);
            EraseFromQueueLocked(key, slot);
        }
        ::CloseHandle(slot->event);
        slot->event = nullptr;

        if (got_quit) {
            return E_ABORT;
        }
        if (canceled && !received) {
            return STG_E_REVERTED;
        }
        if (!received) {
            return E_FAIL;  // timeout
        }
        if (!error.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "DispatcherChunkProvider: parent error: %.96s", error.c_str());
            LH_LOG_WARN(buf);
            return E_FAIL;
        }
        out_data    = std::move(data);
        out_is_last = is_last;
        return S_OK;
    }

    // Pipe thread → STA thread bridge. Finds the matching pending request
    // by the (transfer_id, file_id, offset) tuple, stages bytes, and
    // signals the request's event. Late / stale tuples are dropped quietly
    // — that's how requests cancelled by a superseded clipboard cycle
    // drain.
    void DeliverChunk(const std::string&         transfer_id,
                      const std::string&         file_id,
                      std::uint64_t              offset,
                      std::vector<std::uint8_t>  data,
                      bool                       is_last,
                      const std::string&         error) {
        const std::string key = MakeKey(transfer_id, file_id, offset);
        std::shared_ptr<PendingChunk> slot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = pending_.find(key);
            if (it == pending_.end() || it->second.empty()) {
                return;  // no waiter (timed out / canceled / unknown)
            }
            // FIFO: the oldest pending request wins. Match the wire order
            // by which FetchChunks were issued, which is also the order
            // the parent processes them.
            slot = it->second.front();
            slot->data     = std::move(data);
            slot->is_last  = is_last;
            slot->error    = error;
            slot->received = true;
        }
        if (slot->event) ::SetEvent(slot->event);
    }

    // Wake every IN-FLIGHT FetchChunk so the helper's shutdown /
    // clipboard-ownership-rotation path doesn't hang the STA. Each
    // existing slot is flagged cancelled and its event is set so the
    // FetchChunk wait wakes, sees slot->cancelled, and returns
    // STG_E_REVERTED. Crucially we do NOT install a process-wide gate
    // here — subsequent FetchChunk calls (for a fresh ANNOUNCE_DELAYED)
    // are accepted as normal.
    void CancelAll() {
        std::vector<HANDLE> events;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [_, queue] : pending_) {
                for (auto& slot : queue) {
                    if (slot) {
                        slot->cancelled = true;
                        if (slot->event) events.push_back(slot->event);
                    }
                }
            }
        }
        for (HANDLE h : events) ::SetEvent(h);
    }

private:
    struct PendingChunk {
        HANDLE                       event{nullptr};
        std::vector<std::uint8_t>    data;
        bool                         is_last{false};
        std::string                  error;
        bool                         received{false};
        // Set by CancelAll on existing slots when clipboard ownership
        // rotates or the helper shuts down. Per-slot rather than process-
        // wide so the FetchChunk path can stay open for fresh
        // announcements after a rotation.
        bool                         cancelled{false};
    };

    // Composite key for the in-flight FetchChunk map. The wire fields the
    // parent echoes back (transfer_id, file_id, offset) uniquely identify
    // a request within the current process — file_id is per-announcement
    // and offset prevents collisions when the shell issues sequential
    // Reads on the same stream before the previous one has returned (rare
    // but legal). Format is "<transfer_id>|<file_id>|<decimal-offset>";
    // both transfer_id and file_id are opaque tokens (no embedded '|' is
    // expected, but if one slips through the worst case is a missed
    // correlation that triggers a FetchChunk timeout, not data corruption).
    static std::string MakeKey(const std::string& transfer_id,
                               const std::string& file_id,
                               std::uint64_t      offset) {
        std::string key;
        key.reserve(transfer_id.size() + file_id.size() + 24);
        key.append(transfer_id);
        key.push_back('|');
        key.append(file_id);
        key.push_back('|');
        key.append(std::to_string(offset));
        return key;
    }

    // Remove a specific slot from its per-key queue under mu_. Caller MUST
    // hold mu_. If the queue becomes empty the key is erased so the map
    // doesn't accumulate orphan empty queues across long-running sessions.
    void EraseFromQueueLocked(const std::string& key,
                              const std::shared_ptr<PendingChunk>& slot) {
        auto it = pending_.find(key);
        if (it == pending_.end()) return;
        auto& q = it->second;
        for (auto qit = q.begin(); qit != q.end(); ++qit) {
            if (*qit == slot) { q.erase(qit); break; }
        }
        if (q.empty()) pending_.erase(it);
    }

    PipeServer*                                                pipe_;
    std::mutex                                                 mu_;
    // Per-key FIFO queue of in-flight FetchChunk requests. A queue rather
    // than a single slot so two consumers reading the SAME
    // (transfer_id, file_id, offset) tuple don't strand each other (see
    // FetchChunk staging block). DeliverChunk pops front; FetchChunk
    // cleanup removes its own slot via EraseFromQueueLocked.
    std::unordered_map<std::string, std::deque<std::shared_ptr<PendingChunk>>> pending_;
};

Dispatcher::Dispatcher(StaWorker* sta, PipeServer* pipe)
    : sta_(sta), pipe_(pipe) {
    render_event_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    if (render_event_ == nullptr) {
        LH_LOG_ERROR("Dispatcher: CreateEventW for render_event_ failed; delayed rendering disabled");
    }
    chunk_provider_ = std::make_unique<DispatcherChunkProvider>(pipe);
}

Dispatcher::~Dispatcher() {
    // Both data-object pointers should already be nullptr by this point
    // because wmain calls ReleaseStaBoundResources(&sta) before sta.Stop().
    // If they are not (e.g. the caller forgot to invoke the explicit hook
    // on a shutdown path), fall back to releasing here — the apartment is
    // gone, so this risks a bad call, but leaking is worse and the process
    // is about to exit anyway.
    if (virtual_data_object_) {
        LH_LOG_WARN("Dispatcher::~Dispatcher releasing inbound IDataObject after STA shutdown — "
                    "ReleaseStaBoundResources was not called before sta.Stop");
        ReleaseDataObject(virtual_data_object_);
        virtual_data_object_ = nullptr;
    }
    if (outbound_data_object_) {
        LH_LOG_WARN("Dispatcher::~Dispatcher releasing outbound IDataObject after STA shutdown — "
                    "ReleaseStaBoundResources was not called before sta.Stop");
        outbound_data_object_->Release();
        outbound_data_object_ = nullptr;
    }
    if (render_event_) {
        ::CloseHandle(render_event_);
        render_event_ = nullptr;
    }
}

void Dispatcher::CancelPendingRender() {
    if (render_event_) ::SetEvent(render_event_);
}

void Dispatcher::ReleaseStaBoundResources(StaWorker* sta) {
    if (sta == nullptr) {
        return;
    }
    // 1. Wake any FetchChunk waiter currently blocked inside an
    //    IStream::Read on the STA thread, so the STA can drain and the
    //    later RunSync calls below don't deadlock against it.
    if (chunk_provider_) {
        chunk_provider_->CancelAll();
    }

    // 2. Release the read-side (inbound) IDataObject — the one we
    //    AddRef'd when ReadClipboard observed virtual files on our own
    //    local clipboard. Snapshot under the lock then Release from the
    //    STA thread so the COM call happens in the apartment that owns
    //    the proxy.
    void* in_obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(file_paths_mu_);
        in_obj = virtual_data_object_;
        virtual_data_object_ = nullptr;
        virtual_lindex_.clear();
    }
    if (in_obj != nullptr) {
        try {
            sta->RunSync([in_obj]() {
                ReleaseDataObject(in_obj);
            });
        } catch (const std::future_error&) {
            // StaWorker may already have exited (broken_promise) — release
            // locally as last resort. Leaks are worse than UB at exit.
            LH_LOG_WARN("ReleaseStaBoundResources: STA gone, releasing inbound IDataObject locally");
            ReleaseDataObject(in_obj);
        }
    }

    // 3. Release the write-side (outbound) IDataObject — the one we
    //    handed to OleSetClipboard inside HandleAnnounceDelayed for
    //    content_type=Files. We hold a retained reference in
    //    outbound_data_object_; OLE has its own. Sequence:
    //      a. OleSetClipboard(nullptr) on the STA — OLE drops its ref.
    //      b. Release our retained ref on the STA — final destruction.
    //    Doing both lets the IDataObject's destructor (and any STA-bound
    //    members) wind up inside the apartment that owns the pointer.
    IDataObject* out_obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(file_paths_mu_);
        out_obj = outbound_data_object_;
        outbound_data_object_ = nullptr;
    }
    if (out_obj != nullptr) {
        try {
            sta->RunSync([out_obj]() {
                // Only clear the clipboard if OUR data object is still the
                // current owner. Otherwise a foreign app has taken over
                // since we last checked, and OleSetClipboard(nullptr)
                // would wipe ITS contents — exactly the bug "carefully
                // execute reversible actions" warns against.
                if (::OleIsCurrentClipboard(out_obj) == S_OK) {
                    const HRESULT hr = ::OleSetClipboard(nullptr);
                    if (FAILED(hr)) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf),
                                      "OleSetClipboard(nullptr) failed: hr=0x%08lx", hr);
                        LH_LOG_WARN(buf);
                    }
                }
                out_obj->Release();
            });
        } catch (const std::future_error&) {
            LH_LOG_WARN("ReleaseStaBoundResources: STA gone, releasing outbound IDataObject locally");
            out_obj->Release();
        }
    }

    // 4. Legacy fallback for the classical SetClipboardData owners (Text/
    //    Image delayed rendering). Those paths leave the clipboard owner
    //    set to our STA hwnd; clear it so the next process owning the
    //    helper window's class doesn't see stale advertised formats.
    const HWND owner = sta->HwndOwner();
    if (owner != nullptr) {
        try {
            sta->RunSync([owner]() {
                if (::GetClipboardOwner() == owner) {
                    const HRESULT hr = ::OleSetClipboard(nullptr);
                    if (FAILED(hr)) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf),
                                      "OleSetClipboard(nullptr) failed: hr=0x%08lx", hr);
                        LH_LOG_WARN(buf);
                    }
                }
            });
        } catch (const std::future_error&) {
            // STA already exited; OLE owns the lifetime and will tear
            // things down during process exit.
            LH_LOG_WARN("ReleaseStaBoundResources: STA gone, skipping OleSetClipboard(nullptr)");
        }
    }
}

std::vector<std::uint8_t> Dispatcher::OnConnect() {
    return EncodeReady();
}

void Dispatcher::OnClipboardChanged() {
    // Invoked on the STA thread. Skip the read if WE are the current
    // clipboard owner — that means the change was caused by our own
    // ANNOUNCE_DELAYED / SET_CLIPBOARD call, and reading right now would
    // either (a) re-trigger our own WM_RENDERFORMAT (since the delayed
    // slot is empty), defeating the entire point of lazy paste, or (b)
    // bounce content we just wrote back to the parent. The parent already
    // knows the state it asked us to set; only changes initiated by
    // OTHER apps should be relayed.
    //
    // Two ownership tests are needed because the classical and OLE paths
    // expose different "owner" identities:
    //
    //   1. SetClipboardData / delayed-rendering (Text/Image): the owner is
    //      our STA window (HwndOwner). GetClipboardOwner() == owner.
    //   2. OleSetClipboard (Files virtual files): the owner is OLE's
    //      internal clipboard window, NOT our hwnd. We instead test the
    //      IDataObject pointer with OleIsCurrentClipboard.
    //
    // Both checks must be cheap and side-effect-free; they run on every
    // WM_CLIPBOARDUPDATE delivered by AddClipboardFormatListener.
    {
        IDataObject* obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(file_paths_mu_);
            obj = outbound_data_object_;
        }
        if (obj != nullptr) {
            // S_OK = we are still current. S_FALSE = a foreign app has
            // taken over; OLE has already dropped its ref, so release
            // ours to balance and let the new clipboard owner's contents
            // flow through to the parent on the next branch below.
            if (::OleIsCurrentClipboard(obj) == S_OK) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(file_paths_mu_);
                if (outbound_data_object_ == obj) {
                    outbound_data_object_ = nullptr;
                }
            }
            obj->Release();
            // Also cancel any in-flight FetchChunk waiters — the streams
            // are dead the moment ownership rotated; better to surface
            // STG_E_REVERTED to a still-pumping consumer than let it
            // burn 120 s in MsgWaitForMultipleObjects.
            if (chunk_provider_) chunk_provider_->CancelAll();
        }
    }

    const HWND owner = sta_->HwndOwner();
    if (::GetClipboardOwner() == owner) {
        return;
    }

    ClipboardSnapshot snap;
    if (!ReadClipboard(owner, snap)) {
        // Nothing readable (empty clipboard or Phase-4+ format only) — no
        // CLIPBOARD_CHANGED to ship.
        return;
    }
    // Cache the file paths / virtual-file IDataObject so subsequent
    // FILE_CHUNK_REQUESTs can resolve file_id="N" to either an on-disk
    // path or an IDataObject + lindex.
    if (snap.content_type == ClipboardContentType::Files) {
        if (!snap.virtual_files.empty()) {
            std::vector<std::uint32_t> lindex;
            lindex.reserve(snap.virtual_files.size());
            for (const auto& v : snap.virtual_files) lindex.push_back(v.lindex);
            UpdateVirtualFiles(snap.virtual_data_object, std::move(lindex));
        } else {
            UpdateFilePaths(snap.file_paths);
        }
    }
    const auto inner = EncodeClipboardData(snap);
    const auto frame = EncodeClipboardEnvelope(HelperMessageType::ClipboardChanged, inner);
    if (!pipe_->SendFrame(frame)) {
        LH_LOG_WARN("CLIPBOARD_CHANGED frame dropped: no active client");
    }
}

void Dispatcher::UpdateFilePaths(const std::vector<std::wstring>& paths) {
    std::lock_guard<std::mutex> lock(file_paths_mu_);
    file_paths_.clear();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        file_paths_[std::to_string(static_cast<std::uint64_t>(i))] = paths[i];
    }
    // Releasing the previous virtual IDataObject (if any) is the
    // responsibility of UpdateVirtualFiles when the new snapshot is
    // virtual instead. When the new snapshot is physical, clear too.
    if (virtual_data_object_) {
        ReleaseDataObject(virtual_data_object_);
        virtual_data_object_ = nullptr;
    }
    virtual_lindex_.clear();
}

void Dispatcher::UpdateVirtualFiles(void* data_object, std::vector<std::uint32_t> lindex_by_id) {
    std::lock_guard<std::mutex> lock(file_paths_mu_);
    // A virtual snapshot supersedes any physical one — clear paths.
    file_paths_.clear();
    if (virtual_data_object_) {
        ReleaseDataObject(virtual_data_object_);
    }
    virtual_data_object_ = data_object;  // ownership transferred in
    virtual_lindex_.clear();
    for (std::size_t i = 0; i < lindex_by_id.size(); ++i) {
        virtual_lindex_[std::to_string(static_cast<std::uint64_t>(i))] = lindex_by_id[i];
    }
}

std::vector<std::uint8_t> Dispatcher::Handle(const std::vector<std::uint8_t>& request) {
    ParsedHelperMessage in;
    if (!ParseHelperMessage(request, in)) {
        LH_LOG_WARN("HelperMessage parse failed; dropping frame");
        return {};
    }

    char log_buf[96];
    std::snprintf(log_buf, sizeof(log_buf),
                  "HelperMessage recv: type=%d, payload_size=%zu",
                  static_cast<int>(in.type), request.size());
    LH_LOG_INFO(log_buf);

    switch (in.type) {
        case HelperMessageType::SetClipboard: {
            if (in.clipboard_data_ptr == nullptr) {
                return EncodeError("SET_CLIPBOARD missing clipboard_data");
            }
            ParsedClipboardData pcd;
            if (!ParseClipboardData(in.clipboard_data_ptr, in.clipboard_data_len, pcd)) {
                return EncodeError("SET_CLIPBOARD clipboard_data parse failed");
            }
            const HWND owner = sta_->HwndOwner();

            switch (pcd.content_type) {
                case ClipboardContentType::Text: {
                    std::string utf8(reinterpret_cast<const char*>(pcd.payload_ptr), pcd.payload_len);
                    std::wstring wide = Utf8ToWide(utf8);
                    const bool ok = sta_->RunSync([owner, &wide]() {
                        return WriteClipboardText(owner, wide);
                    });
                    if (!ok) return EncodeError("WriteClipboardText failed");
                    return {};
                }
                case ClipboardContentType::Image: {
                    const std::uint8_t* png  = pcd.payload_ptr;
                    const std::size_t   plen = pcd.payload_len;
                    const bool ok = sta_->RunSync([owner, png, plen]() {
                        return WriteClipboardImagePng(owner, png, plen);
                    });
                    if (!ok) return EncodeError("WriteClipboardImagePng failed");
                    return {};
                }
                case ClipboardContentType::Files: {
                    // SET_CLIPBOARD content_type=Files: paths arrive as the
                    // repeated `files` FileMetadata sub-messages, NOT in
                    // `payload`. (Matches macOS clipboard_darwin.go's
                    // contentToProto output and our own EncodeFileMetadata
                    // on the GET reply side.) For each file we take
                    // relative_path (field 3) as the absolute Win32 path.
                    std::vector<std::wstring> paths;
                    paths.reserve(pcd.files.size());
                    for (const auto& [ptr, len] : pcd.files) {
                        std::string rel;
                        if (ExtractRelativePath(ptr, len, rel) && !rel.empty()) {
                            paths.push_back(Utf8ToWide(rel));
                        }
                    }
                    if (paths.empty()) {
                        LH_LOG_INFO("SET_CLIPBOARD files: no usable file metadata, skipping");
                        return {};
                    }
                    const bool ok = sta_->RunSync([owner, &paths]() {
                        return WriteClipboardFiles(owner, paths);
                    });
                    if (!ok) return EncodeError("WriteClipboardFiles failed");
                    return {};
                }
                case ClipboardContentType::Unspecified:
                default:
                    LH_LOG_INFO("SET_CLIPBOARD unknown content type; ignoring");
                    return {};
            }
        }

        case HelperMessageType::GetClipboard: {
            const HWND owner = sta_->HwndOwner();
            ClipboardSnapshot snap;
            const bool got = sta_->RunSync([owner, &snap]() {
                return ReadClipboard(owner, snap);
            });
            if (!got) {
                // Empty / unsupported format. Reply with an empty
                // CLIPBOARD_CONTENT envelope so the parent can disambiguate
                // "no clipboard" from "no helper response".
                return EncodeClipboardEnvelope(HelperMessageType::ClipboardContent, {});
            }
            if (snap.content_type == ClipboardContentType::Files) {
                if (!snap.virtual_files.empty()) {
                    std::vector<std::uint32_t> lindex;
                    lindex.reserve(snap.virtual_files.size());
                    for (const auto& v : snap.virtual_files) lindex.push_back(v.lindex);
                    UpdateVirtualFiles(snap.virtual_data_object, std::move(lindex));
                } else {
                    UpdateFilePaths(snap.file_paths);
                }
            }
            const auto inner = EncodeClipboardData(snap);
            return EncodeClipboardEnvelope(HelperMessageType::ClipboardContent, inner);
        }

        case HelperMessageType::AnnounceDelayed: {
            if (in.announcement_ptr == nullptr) {
                return EncodeError("ANNOUNCE_DELAYED missing announcement payload");
            }
            HandleAnnounceDelayed(
                std::vector<std::uint8_t>(in.announcement_ptr,
                                          in.announcement_ptr + in.announcement_len));
            return {};
        }
        case HelperMessageType::ProvideData: {
            if (in.provide_data_ptr == nullptr) {
                return EncodeError("PROVIDE_DATA missing provide_data payload");
            }
            HandleProvideData(
                std::vector<std::uint8_t>(in.provide_data_ptr,
                                          in.provide_data_ptr + in.provide_data_len));
            return {};
        }
        case HelperMessageType::FileChunkRequest: {
            if (in.file_chunk_request_ptr == nullptr) {
                return EncodeError("FILE_CHUNK_REQUEST missing file_chunk_request payload");
            }
            HandleFileChunkRequest(
                std::vector<std::uint8_t>(in.file_chunk_request_ptr,
                                          in.file_chunk_request_ptr + in.file_chunk_request_len));
            return {};
        }
        case HelperMessageType::FileChunkData: {
            if (in.file_chunk_data_ptr == nullptr) {
                return EncodeError("FILE_CHUNK_DATA missing file_chunk_data payload");
            }
            HandleFileChunkData(
                std::vector<std::uint8_t>(in.file_chunk_data_ptr,
                                          in.file_chunk_data_ptr + in.file_chunk_data_len));
            return {};
        }
        case HelperMessageType::FileTransferProgress:
            // Parent → Helper progress notification; helper currently has
            // nowhere to surface it. Phase 4c may use it for a tray UI.
            LH_LOG_INFO("FILE_TRANSFER_PROGRESS received; UI surface pending");
            return {};

        case HelperMessageType::Shutdown:
            // SHUTDOWN means the parent process is about to close us. Tell
            // the PipeServer loop to wind down so the helper process exits
            // promptly instead of hanging around to accept the next pipe
            // client. Otherwise the parent's subsequent NewClipboardSync
            // would race the still-alive helper for the
            // FILE_FLAG_FIRST_PIPE_INSTANCE slot and fall into a 10 s
            // dial-retry loop — observed end-to-end as a session
            // reconnect storm on the parent side.
            LH_LOG_INFO("SHUTDOWN received; stopping helper");
            if (pipe_) pipe_->Stop();
            return {};

        case HelperMessageType::Unspecified:
        default:
            return EncodeError("unknown or unsupported HelperMessageType");
    }
}

void Dispatcher::HandleAnnounceDelayed(const std::vector<std::uint8_t>& sub_payload) {
    ParsedAnnouncement ann;
    if (!ParseAnnouncement(sub_payload.data(), sub_payload.size(), ann)) {
        LH_LOG_WARN("ANNOUNCE_DELAYED parse failed; dropping");
        return;
    }
    // Persist the pending announcement *before* we tell the OS we own the
    // delayed format, so a fast WM_RENDERFORMAT (which can fire while
    // AnnounceDelayedFormatsForType is still inside its OpenClipboard
    // window) can find a matching hash.
    //
    // ResetEvent + cache-invalidate happen ONLY here, not inside
    // OnRenderFormat. Otherwise a fast PROVIDE_DATA arriving between
    // two WM_RENDERFORMAT invocations for the same announcement (e.g. an
    // app asking for CF_DIBV5 then CF_DIB) would lose its signal — the
    // second WM_RENDERFORMAT would ResetEvent after PROVIDE_DATA had
    // already SetEvent'd it.
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        pending_hash_     = ann.content_hash;
        pending_type_     = ann.content_type;
        pending_response_.clear();
        cached_response_.clear();
        cached_valid_ = false;
        if (render_event_) ::ResetEvent(render_event_);
    }

    const HWND owner = sta_->HwndOwner();
    const auto ct = ann.content_type;

    // Phase 2 cutover: Files content takes the virtual-file IDataObject
    // path. Text and Image continue to use the classical
    // SetClipboardData(fmt, NULL) + WM_RENDERFORMAT format-slot model —
    // that path is fine because the response payloads are small (an entire
    // text/image fits in one DATA_REQUEST round-trip) and Explorer never
    // calls WM_RENDERFORMAT for text/image owners with progress UI.
    if (ct == ClipboardContentType::Files) {
        // Build VirtualFileSpec list from ann.files. Empty file lists are
        // ignored: there is nothing useful to publish, and OleSetClipboard
        // with an IDataObject that advertises zero items confuses
        // consumers.
        std::vector<VirtualFileSpec> specs;
        specs.reserve(ann.files.size());
        for (const auto& [p, n] : ann.files) {
            VirtualFileSpec spec;
            if (ParseFileMetadataForVirtualFile(p, n, spec)) {
                specs.push_back(std::move(spec));
            }
        }
        if (specs.empty()) {
            LH_LOG_WARN("ANNOUNCE_DELAYED Files: no usable file metadata, dropping");
            return;
        }
        if (ann.transfer_id.empty()) {
            // Without a transfer_id the chunk-request reply cannot be
            // routed back to shen's WebRTC source. Bail rather than
            // publish an IDataObject that would later return E_FAIL on
            // every Read.
            LH_LOG_WARN("ANNOUNCE_DELAYED Files: missing transfer_id "
                        "(clipboard.proto ClipboardAnnouncement field 7); "
                        "dropping virtual-file publish");
            return;
        }

        IDataObject* dataobj = nullptr;
        const HRESULT cr = CreateVirtualClipboardDataObject(std::move(specs),
                                                            ann.transfer_id,
                                                            chunk_provider_.get(),
                                                            &dataobj);
        if (FAILED(cr) || dataobj == nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "CreateVirtualClipboardDataObject failed: hr=0x%08lx", cr);
            LH_LOG_WARN(buf);
            return;
        }

        // The OleSetClipboard call AND the outbound_data_object_ swap MUST
        // run atomically on the STA thread. Doing the swap on the pipe
        // thread first would open a UAF window where OnClipboardChanged
        // (also STA-bound, but dispatched between OleSetClipboard pre- and
        // post-state) could observe outbound_data_object_ == dataobj while
        // OleIsCurrentClipboard(dataobj) still returns S_FALSE (we haven't
        // OleSetClipboard'd yet), erroneously Release dataobj, and let the
        // STA later call OleSetClipboard on freed memory. Bundling both
        // operations in the same STA work item closes that window because
        // the STA cannot dispatch WM_CLIPBOARDUPDATE while it's busy
        // executing this lambda. (Gemini code review #1, Phase 2.)
        IDataObject* prev_outbound = nullptr;
        const HRESULT sr = sta_->RunSync(
            [this, dataobj, &prev_outbound]() -> HRESULT {
                const HRESULT hr = ::OleSetClipboard(dataobj);
                if (FAILED(hr)) return hr;
                // OLE has AddRefed dataobj; record our retained ref and
                // capture the prior outbound so the caller can release it
                // outside the lambda (Release-on-STA-thread is fine but
                // doing it here would risk re-entrant clipboard events
                // before WM_CLIPBOARDUPDATE for our own OleSetClipboard
                // has fired).
                std::lock_guard<std::mutex> lock(file_paths_mu_);
                prev_outbound        = outbound_data_object_;
                outbound_data_object_ = dataobj;
                return S_OK;
            });
        if (FAILED(sr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "OleSetClipboard failed: hr=0x%08lx", sr);
            LH_LOG_WARN(buf);
            // OLE didn't take and we never swapped; release our refcount.
            sta_->RunSync([dataobj]() { dataobj->Release(); });
            return;
        }

        // OleSetClipboard succeeded — the previous outbound IDataObject
        // (if any) has lost ownership. OLE released its ref to it the
        // moment we OleSetClipboard'd the new one; release our retained
        // ref now to balance. RunSync onto the STA so the COM call lands
        // in the apartment that owns the pointer.
        if (prev_outbound != nullptr) {
            sta_->RunSync([prev_outbound]() { prev_outbound->Release(); });
        }

        char buf[160];
        const std::string& h = ann.content_hash;
        std::snprintf(buf, sizeof(buf),
                      "ANNOUNCE_DELAYED Files accepted via OleSetClipboard: hash=%.8s%s, files=%zu, transfer_id=%.16s",
                      h.c_str(),
                      h.size() > 8 ? "…" : "",
                      ann.files.size(),
                      ann.transfer_id.c_str());
        LH_LOG_INFO(buf);
        return;
    }

    // Text / Image: classical delayed-rendering path.
    const bool ok = sta_->RunSync([owner, ct]() {
        return AnnounceDelayedFormatsForType(owner, ct);
    });
    if (!ok) {
        LH_LOG_WARN("AnnounceDelayedFormatsForType failed");
        return;
    }
    char buf[160];
    const std::string& h = ann.content_hash;
    std::snprintf(buf, sizeof(buf),
                  "ANNOUNCE_DELAYED accepted: type=%d, hash=%.8s%s",
                  static_cast<int>(ct),
                  h.c_str(),
                  h.size() > 8 ? "…" : "");
    LH_LOG_INFO(buf);
}

void Dispatcher::HandleProvideData(const std::vector<std::uint8_t>& sub_payload) {
    ParsedProvideData pd;
    if (!ParseProvideData(sub_payload.data(), sub_payload.size(), pd)) {
        LH_LOG_WARN("PROVIDE_DATA parse failed; dropping");
        return;
    }

    std::lock_guard<std::mutex> lock(render_mu_);
    if (pending_hash_.empty()) {
        LH_LOG_WARN("PROVIDE_DATA received but no render is pending; dropping");
        return;
    }
    if (pd.content_hash != pending_hash_) {
        // Stale content from a superseded announcement — exactly the
        // failure-mode the macOS helper hit in April 2026. Drop on the
        // floor so the WM_RENDERFORMAT waiter eventually times out
        // instead of serving stale bytes.
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "PROVIDE_DATA hash mismatch: got=%.8s pending=%.8s — discarding",
                      pd.content_hash.c_str(), pending_hash_.c_str());
        LH_LOG_WARN(buf);
        return;
    }
    pending_response_ = std::move(pd.data);
    if (render_event_) ::SetEvent(render_event_);
}

void Dispatcher::OnRenderFormat(unsigned int format, bool cache_only) {
    // Determine which ContentType this Win32 format maps to. Phase 2:
    // CF_HDROP is no longer advertised by us — Files content_type goes
    // through OleSetClipboard with a virtual-file IDataObject instead, so
    // a WM_RENDERFORMAT for CF_HDROP can only fire if something stale is
    // in flight. Bail with a warning rather than round-trip to the parent.
    ClipboardContentType requested = ClipboardContentType::Unspecified;
    if (format == CF_UNICODETEXT) {
        requested = ClipboardContentType::Text;
    } else if (format == CF_DIBV5 || format == CF_DIB) {
        requested = ClipboardContentType::Image;
    } else {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WM_RENDERFORMAT: unsupported format 0x%x", format);
        LH_LOG_WARN(buf);
        return;
    }

    // Snapshot the pending announcement under render_mu_; if we already
    // have a cached response from a prior render for this same hash (e.g.
    // because an app just asked us for CF_DIBV5 and is now asking for
    // CF_DIB), reuse the bytes without going back to the parent. This
    // short-circuit is what keeps WM_RENDERALLFORMATS from burning a
    // 120 s wait per advertised format on the way out.
    std::string hash;
    ClipboardContentType pending_type = ClipboardContentType::Unspecified;
    std::vector<std::uint8_t> response;
    bool have_data = false;
    {
        std::lock_guard<std::mutex> lock(render_mu_);
        hash         = pending_hash_;
        pending_type = pending_type_;
        if (hash.empty()) {
            LH_LOG_WARN("WM_RENDERFORMAT but no pending announcement; leaving slot empty");
            return;
        }
        if (pending_type != requested) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "WM_RENDERFORMAT type mismatch: format=0x%x asks type=%d, pending=%d",
                          format, static_cast<int>(requested), static_cast<int>(pending_type));
            LH_LOG_WARN(buf);
            return;
        }
        if (cached_valid_) {
            response = cached_response_;
            have_data = true;
        } else if (!pending_response_.empty()) {
            // PROVIDE_DATA arrived BEFORE the OS sent us WM_RENDERFORMAT
            // (rare but possible). Adopt the response now and skip the
            // round-trip.
            response = std::move(pending_response_);
            cached_response_ = response;
            cached_valid_ = true;
            have_data = true;
        }
    }

    if (!have_data) {
        // WM_RENDERALLFORMATS path: we cannot afford the 120s wait per
        // advertised format because we are about to lose ownership and
        // the system clipboard would be frozen for minutes. Skip silently;
        // apps that paste during the brief ownership-handoff window will
        // see an empty slot, which is the documented WM_RENDERALLFORMATS
        // semantics anyway.
        if (cache_only) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "WM_RENDERALLFORMATS: no cached data for format 0x%x; leaving slot empty",
                          format);
            LH_LOG_INFO(buf);
            return;
        }

        // Send DATA_REQUEST to the parent. PipeServer::SendFrame is
        // safe from any thread; the STA thread we are on is fine.
        const auto frame = EncodeDataRequest(hash, requested);
        if (!pipe_->SendFrame(frame)) {
            LH_LOG_WARN("WM_RENDERFORMAT: DATA_REQUEST send failed (no active client)");
            return;
        }

        if (render_event_ == nullptr) return;

        // Wait via MsgWaitForMultipleObjects so the STA still pumps
        // window messages. The event is set by HandleProvideData on the
        // pipe thread, which means by the time we wake we may have to
        // share the response with a re-entrant WM_RENDERFORMAT that ran
        // during DispatchMessage — that's exactly why we cache.
        const DWORD start = ::GetTickCount();
        while (true) {
            const DWORD elapsed = ::GetTickCount() - start;
            if (elapsed >= kRenderTimeoutMs) {
                LH_LOG_WARN("WM_RENDERFORMAT: timed out waiting for PROVIDE_DATA");
                return;
            }
            const DWORD remaining = kRenderTimeoutMs - elapsed;
            const DWORD wait_result = ::MsgWaitForMultipleObjects(
                1, &render_event_, FALSE, remaining, QS_ALLINPUT);
            if (wait_result == WAIT_OBJECT_0) {
                break;
            }
            if (wait_result == WAIT_OBJECT_0 + 1) {
                MSG msg{};
                bool got_quit = false;
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        // The outer PumpMessages relies on GetMessage
                        // returning 0 to exit. PeekMessage(PM_REMOVE) on
                        // WM_QUIT consumes it silently, so we re-post it
                        // and stop pumping — otherwise sta.Stop() would
                        // hang up to 120 s waiting for render_event_.
                        ::PostQuitMessage(static_cast<int>(msg.wParam));
                        got_quit = true;
                        break;
                    }
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                if (got_quit) {
                    LH_LOG_INFO("WM_RENDERFORMAT: WM_QUIT observed mid-wait; aborting render");
                    return;
                }
                // After the pump runs, a nested WM_RENDERFORMAT may have
                // adopted pending_response_ into cached_response_ already
                // — check before sleeping again.
                std::lock_guard<std::mutex> lock(render_mu_);
                if (pending_hash_ != hash) {
                    LH_LOG_WARN("WM_RENDERFORMAT: announcement superseded during wait; aborting");
                    return;
                }
                if (cached_valid_) {
                    response = cached_response_;
                    have_data = true;
                    break;
                }
                continue;
            }
            if (wait_result == WAIT_TIMEOUT) {
                LH_LOG_WARN("WM_RENDERFORMAT: timed out waiting for PROVIDE_DATA");
                return;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "MsgWaitForMultipleObjects failed: 0x%lx", wait_result);
            LH_LOG_ERROR(buf);
            return;
        }

        if (!have_data) {
            std::lock_guard<std::mutex> lock(render_mu_);
            // Verify we are still rendering for the announcement we started
            // with. A racing HandleAnnounceDelayed may have rotated
            // pending_hash_ and refilled pending_response_ with bytes
            // intended for a DIFFERENT content — rendering those for our
            // original format would leak stale content to the paste app.
            if (pending_hash_ != hash) {
                LH_LOG_WARN("WM_RENDERFORMAT: announcement superseded after wake; aborting");
                return;
            }
            if (!pending_response_.empty()) {
                response = std::move(pending_response_);
                cached_response_ = response;
                cached_valid_ = true;
            } else if (cached_valid_) {
                response = cached_response_;
            } else {
                LH_LOG_WARN("WM_RENDERFORMAT: event signaled but no response in flight");
                return;
            }
        }
    }

    switch (requested) {
        case ClipboardContentType::Text: {
            std::string utf8(reinterpret_cast<const char*>(response.data()), response.size());
            const std::wstring wide = Utf8ToWide(utf8);
            if (!RenderTextDuringWmRenderFormat(wide)) {
                LH_LOG_WARN("RenderTextDuringWmRenderFormat failed");
            }
            break;
        }
        case ClipboardContentType::Image:
            if (!RenderImageDuringWmRenderFormat(format, response.data(), response.size())) {
                LH_LOG_WARN("RenderImageDuringWmRenderFormat failed");
            }
            break;
        case ClipboardContentType::Files:
            // Files no longer flow through WM_RENDERFORMAT — they're served
            // via OleSetClipboard + IStream::Read. If we ever observe a
            // pending render of type Files here, an announcement upstream
            // never flipped over to the new path. Log and drop.
            LH_LOG_WARN("OnRenderFormat: Files pending in WM_RENDERFORMAT path "
                        "— this should be handled via OleSetClipboard");
            break;
        default:
            break;
    }
}

namespace {

struct ParsedFileChunkRequest {
    std::string   transfer_id;
    std::string   file_id;
    std::uint64_t offset{0};
    std::uint32_t size{0};
};

bool ParseFileChunkRequest(const std::uint8_t* data, std::size_t len, ParsedFileChunkRequest& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        switch (field) {
            case proto::kFCReqFieldTransferId:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p=nullptr; std::size_t n=0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.transfer_id.assign(reinterpret_cast<const char*>(p), n); }
                continue;
            case proto::kFCReqFieldFileId:
                if (wire != WireType::LengthDelim) { if (!r.SkipField(wire)) return false; continue; }
                { const std::uint8_t* p=nullptr; std::size_t n=0;
                  if (!r.ReadLengthDelim(p, n)) return false;
                  out.file_id.assign(reinterpret_cast<const char*>(p), n); }
                continue;
            case proto::kFCReqFieldOffset:
                if (wire != WireType::Varint) { if (!r.SkipField(wire)) return false; continue; }
                { std::uint64_t v=0; if (!r.ReadVarint(v)) return false; out.offset = v; }
                continue;
            case proto::kFCReqFieldSize:
                if (wire != WireType::Varint) { if (!r.SkipField(wire)) return false; continue; }
                { std::uint64_t v=0; if (!r.ReadVarint(v)) return false;
                  out.size = static_cast<std::uint32_t>(v); }
                continue;
            default:
                if (!r.SkipField(wire)) return false;
                continue;
        }
    }
    return true;
}

// Encode an outbound HelperFileChunkData envelope. Returns the full
// HelperMessage payload (already framed for pipe.SendFrame).
std::vector<std::uint8_t> EncodeFileChunkData(const std::string& transfer_id,
                                              const std::string& file_id,
                                              std::uint64_t offset,
                                              const std::uint8_t* data,
                                              std::size_t data_len,
                                              bool is_last,
                                              std::string_view error_msg) {
    Writer inner;
    inner.WriteStringField(proto::kFCDataFieldTransferId, transfer_id);
    inner.WriteStringField(proto::kFCDataFieldFileId, file_id);
    inner.WriteUint64Field(proto::kFCDataFieldOffset, offset);
    inner.WriteBytesField(proto::kFCDataFieldData, data, data_len);
    inner.WriteBoolField(proto::kFCDataFieldIsLast, is_last);
    if (!error_msg.empty()) {
        inner.WriteStringField(proto::kFCDataFieldError, error_msg);
    }

    Writer out;
    out.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::FileChunkData));
    out.WriteSubMessageField(proto::kFieldFileChunkData, inner.bytes());
    out.WriteUint64Field(proto::kFieldTimestamp, NowUnixMillis());
    return out.take();
}

constexpr std::uint32_t kDefaultChunkSize = 256 * 1024;   // 256 KiB matches macOS helper
constexpr std::uint32_t kMaxChunkSize     = 4   * 1024 * 1024;  // 4 MiB hard cap

}  // namespace

void Dispatcher::HandleFileChunkRequest(const std::vector<std::uint8_t>& sub_payload) {
    ParsedFileChunkRequest req;
    if (!ParseFileChunkRequest(sub_payload.data(), sub_payload.size(), req)) {
        LH_LOG_WARN("FILE_CHUNK_REQUEST parse failed; dropping");
        return;
    }
    std::uint32_t want = req.size;
    if (want == 0)        want = kDefaultChunkSize;
    if (want > kMaxChunkSize) want = kMaxChunkSize;

    // Resolve file_id. Two backing kinds:
    //   - Physical: file_paths_ has an absolute Win32 path. Read off disk
    //     from any thread.
    //   - Virtual: virtual_data_object_ + virtual_lindex_. Must call
    //     IDataObject::GetData on the STA thread.
    std::wstring path;
    void*        vdo = nullptr;
    std::uint32_t vlindex = 0;
    bool is_virtual = false;
    {
        std::lock_guard<std::mutex> lock(file_paths_mu_);
        auto it = file_paths_.find(req.file_id);
        if (it != file_paths_.end()) {
            path = it->second;
        } else {
            auto vit = virtual_lindex_.find(req.file_id);
            if (vit != virtual_lindex_.end() && virtual_data_object_ != nullptr) {
                vdo        = virtual_data_object_;
                vlindex    = vit->second;
                is_virtual = true;
                // Pin the IDataObject under the lock so a concurrent
                // UpdateVirtualFiles (firing from OnClipboardChanged on
                // the STA thread) cannot Release it out from under the
                // RunSync lambda we are about to schedule. We Release
                // our extra reference after the lambda returns.
                AddRefDataObject(vdo);
            }
        }
    }
    if (!is_virtual && path.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "FILE_CHUNK_REQUEST: unknown file_id=%s", req.file_id.c_str());
        LH_LOG_WARN(buf);
        const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               nullptr, 0, true, "unknown file_id");
        (void)pipe_->SendFrame(frame);
        return;
    }

    if (is_virtual) {
        std::vector<std::uint8_t> buf;
        bool is_last = false;
        const std::uint64_t offset = req.offset;
        const std::uint32_t want_size = want;
        const bool ok = sta_->RunSync([vdo, vlindex, offset, want_size, &buf, &is_last]() {
            return ReadVirtualFileChunk(vdo, vlindex, offset, want_size, buf, is_last);
        });
        // Release the AddRef we took inside file_paths_mu_. After this
        // line `vdo` is invalid for use; the in-flight read has already
        // completed inside the lambda.
        ReleaseDataObject(vdo);
        if (!ok) {
            const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                                   nullptr, 0, true, "virtual file read failed");
            (void)pipe_->SendFrame(frame);
            return;
        }
        const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               buf.data(), buf.size(), is_last, {});
        (void)pipe_->SendFrame(frame);
        return;
    }

    // Open shared-read so concurrent paste/preview operations on the same
    // file don't lock us out. CreateFileW + ReadFile is simpler than
    // std::ifstream for Win32 paths (handles >MAX_PATH after manifesto and
    // wide chars natively).
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "FILE_CHUNK_REQUEST: CreateFileW failed: lastError=%lu", ::GetLastError());
        LH_LOG_WARN(buf);
        const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               nullptr, 0, true, "open failed");
        (void)pipe_->SendFrame(frame);
        return;
    }

    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(req.offset);
    if (!::SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        ::CloseHandle(h);
        const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               nullptr, 0, true, "seek failed");
        (void)pipe_->SendFrame(frame);
        return;
    }

    std::vector<std::uint8_t> buf(want);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, buf.data(), want, &got, nullptr);
    if (!ok) {
        ::CloseHandle(h);
        const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                               nullptr, 0, true, "read failed");
        (void)pipe_->SendFrame(frame);
        return;
    }

    // is_last when ReadFile returned fewer bytes than requested (i.e. we
    // hit EOF). This is exactly the macOS helper's convention so the Go
    // FileChunkReader call boundary stays identical.
    const bool is_last = (got < want);
    buf.resize(got);

    const auto frame = EncodeFileChunkData(req.transfer_id, req.file_id, req.offset,
                                           buf.data(), buf.size(), is_last, {});
    if (!pipe_->SendFrame(frame)) {
        LH_LOG_WARN("FILE_CHUNK_REQUEST: FILE_CHUNK_DATA send failed (no active client)");
    }
    ::CloseHandle(h);
}

void Dispatcher::HandleFileChunkData(const std::vector<std::uint8_t>& sub_payload) {
    ParsedFileChunkData chunk;
    if (!ParseFileChunkData(sub_payload.data(), sub_payload.size(), chunk)) {
        LH_LOG_WARN("FILE_CHUNK_DATA parse failed; dropping");
        return;
    }
    if (chunk_provider_ == nullptr) {
        LH_LOG_WARN("FILE_CHUNK_DATA received but chunk_provider_ is null; dropping");
        return;
    }
    chunk_provider_->DeliverChunk(chunk.transfer_id, chunk.file_id, chunk.offset,
                                  std::move(chunk.data), chunk.is_last, chunk.error);
}

}  // namespace leviathan::clipboard_helper
