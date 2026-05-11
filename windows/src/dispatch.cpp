#include "dispatch.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "clipboard_ops.h"
#include "helper_proto.h"
#include "log.h"
#include "pipe_server.h"
#include "proto_wire.h"
#include "sta_worker.h"

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
// repeated `files` sub-message of ClipboardData.
constexpr std::uint32_t kFMFieldFileId       = 1;
constexpr std::uint32_t kFMFieldFilename     = 2;
constexpr std::uint32_t kFMFieldRelativePath = 3;
constexpr std::uint32_t kFMFieldFileSize     = 4;

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
        if (!r.SkipField(wire)) return false;
    }
    return true;
}

// Parse just enough of a ClipboardData sub-message to extract the content
// type and the payload bytes (UTF-8 text in Phase 3a).
struct ParsedClipboardData {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    const std::uint8_t*  payload_ptr{nullptr};
    std::size_t          payload_len{0};
};

struct ParsedAnnouncement {
    ClipboardContentType content_type{ClipboardContentType::Unspecified};
    std::string          content_hash;
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
        if (!r.SkipField(wire)) return false;
    }
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
        if (!r.SkipField(wire)) return false;
    }
    return true;
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

}  // namespace

Dispatcher::Dispatcher(StaWorker* sta, PipeServer* pipe)
    : sta_(sta), pipe_(pipe) {
    render_event_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    if (render_event_ == nullptr) {
        LH_LOG_ERROR("Dispatcher: CreateEventW for render_event_ failed; delayed rendering disabled");
    }
}

void Dispatcher::CancelPendingRender() {
    if (render_event_) ::SetEvent(render_event_);
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
                    // For SET_CLIPBOARD Files we currently only honour the
                    // FileMetadata-derived paths if the parent embedded them
                    // in payload (newline-separated UTF-8). The common
                    // case for Files is delayed rendering via
                    // ANNOUNCE_DELAYED + WM_RENDERFORMAT instead.
                    std::string utf8(reinterpret_cast<const char*>(pcd.payload_ptr), pcd.payload_len);
                    std::vector<std::wstring> paths;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= utf8.size(); ++i) {
                        if (i == utf8.size() || utf8[i] == '\n') {
                            if (i > start) paths.push_back(Utf8ToWide(utf8.substr(start, i - start)));
                            start = i + 1;
                        }
                    }
                    if (paths.empty()) {
                        // Empty payload — treat as "advertise no files",
                        // which is a no-op. Phase 4c (virtual files) will
                        // route through a separate code path.
                        LH_LOG_INFO("SET_CLIPBOARD files: empty payload, skipping");
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
        case HelperMessageType::FileTransferProgress:
            // Parent → Helper progress notification; helper currently has
            // nowhere to surface it. Phase 4c may use it for a tray UI.
            LH_LOG_INFO("FILE_TRANSFER_PROGRESS received; UI surface pending");
            return {};

        case HelperMessageType::Shutdown:
            LH_LOG_INFO("SHUTDOWN received; caller will close the pipe");
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

void Dispatcher::OnRenderFormat(unsigned int format) {
    // Determine which ContentType this Win32 format maps to.
    ClipboardContentType requested = ClipboardContentType::Unspecified;
    if (format == CF_UNICODETEXT) {
        requested = ClipboardContentType::Text;
    } else if (format == CF_DIBV5 || format == CF_DIB) {
        requested = ClipboardContentType::Image;
    } else if (format == CF_HDROP) {
        requested = ClipboardContentType::Files;
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
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                // After the pump runs, a nested WM_RENDERFORMAT may have
                // adopted pending_response_ into cached_response_ already
                // — check before sleeping again.
                std::lock_guard<std::mutex> lock(render_mu_);
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
            if (!RenderFilesDuringWmRenderFormat(response.data(), response.size())) {
                LH_LOG_WARN("RenderFilesDuringWmRenderFormat failed");
            }
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

}  // namespace leviathan::clipboard_helper
