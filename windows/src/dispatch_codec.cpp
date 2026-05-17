// dispatch_codec — protobuf encode/decode implementations split out of
// dispatch.cpp so the wire-format layer can be unit-tested without
// pulling in OLE / WIC / STA from the surrounding Dispatcher class.
//
// The Dispatcher (clipboard ownership, WM_RENDERFORMAT, IDataObject
// lifetime) stays in dispatch.cpp; this TU only knows about
// HelperMessage bytes in and HelperMessage bytes out.

#include "dispatch_codec.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <utility>

#include "clipboard_ops.h"   // Utf8ToWide / WideToUtf8 declarations
#include "helper_proto.h"
#include "proto_wire.h"

namespace leviathan::clipboard_helper {
namespace dispatch_codec {

using proto::ClipboardContentType;
using proto::HelperMessageType;
using proto_wire::Reader;
using proto_wire::WireType;
using proto_wire::Writer;

// ─── Clock ────────────────────────────────────────────────────────────────

std::uint64_t NowUnixMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ─── Encoders ─────────────────────────────────────────────────────────────

std::vector<std::uint8_t> EncodeReady(ClockFn clock) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Ready));
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeError(std::string_view message, ClockFn clock) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::Error));
    if (!message.empty()) {
        w.WriteStringField(proto::kFieldErrorMessage, message);
    }
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

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

std::vector<std::uint8_t> EncodeClipboardEnvelope(HelperMessageType outer,
                                                  const std::vector<std::uint8_t>& clipboard_data,
                                                  ClockFn clock) {
    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(outer));
    w.WriteSubMessageField(proto::kFieldClipboardData, clipboard_data);
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeDataRequest(std::string_view content_hash,
                                            ClipboardContentType content_type,
                                            ClockFn clock) {
    Writer inner;
    inner.WriteStringField(proto::kReqFieldContentHash, content_hash);
    inner.WriteEnumField(proto::kReqFieldContentType, static_cast<std::int32_t>(content_type));

    Writer w;
    w.WriteEnumField(proto::kFieldType, static_cast<std::int32_t>(HelperMessageType::DataRequest));
    w.WriteSubMessageField(proto::kFieldDataRequest, inner.bytes());
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeFileChunkRequestOutbound(const std::string& transfer_id,
                                                         const std::string& file_id,
                                                         std::uint64_t      offset,
                                                         std::uint32_t      size,
                                                         ClockFn clock) {
    Writer inner;
    inner.WriteStringField(proto::kFCReqFieldTransferId, transfer_id);
    inner.WriteStringField(proto::kFCReqFieldFileId, file_id);
    inner.WriteUint64Field(proto::kFCReqFieldOffset, offset);
    inner.WriteUint64Field(proto::kFCReqFieldSize, size);

    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::FileChunkRequest));
    w.WriteSubMessageField(proto::kFieldFileChunkRequest, inner.bytes());
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeFileChunkData(const std::string& transfer_id,
                                              const std::string& file_id,
                                              std::uint64_t       offset,
                                              const std::uint8_t* data,
                                              std::size_t         data_len,
                                              bool                is_last,
                                              std::string_view    error_msg,
                                              ClockFn clock) {
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
    out.WriteUint64Field(proto::kFieldTimestamp, clock());
    return out.take();
}

// ─── Decoders ─────────────────────────────────────────────────────────────

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

bool ExtractRelativePath(const std::uint8_t* data, std::size_t len, std::string& out) {
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        // FileMetadata field 3 = relative_path (string).
        if (field == kFMFieldRelativePath && wire == WireType::LengthDelim) {
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

}  // namespace dispatch_codec
}  // namespace leviathan::clipboard_helper
