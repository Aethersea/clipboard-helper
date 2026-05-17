#include "dispatch_codec.h"

#include <chrono>

#include "proto_wire.h"

namespace leviathan::clipboard_helper::dispatch_codec {

using proto::ClipboardContentType;
using proto::HelperMessageType;
using proto_wire::Reader;
using proto_wire::WireType;
using proto_wire::Writer;

std::uint64_t NowUnixMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ─── Encoders ────────────────────────────────────────────────────────────

std::vector<std::uint8_t> EncodeReady(ClockFn clock) {
    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::Ready));
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeError(std::string_view message, ClockFn clock) {
    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::Error));
    if (!message.empty()) {
        w.WriteStringField(proto::kFieldErrorMessage, message);
    }
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

// Build an inner ClipboardData (text variant) sub-message.
static std::vector<std::uint8_t> EncodeClipboardDataInnerText(std::string_view utf8_payload,
                                                              std::string_view content_hash) {
    Writer w;
    w.WriteEnumField(proto::kCDFieldContentType,
                     static_cast<std::int32_t>(ClipboardContentType::Text));
    if (!utf8_payload.empty()) {
        w.WriteBytesField(proto::kCDFieldPayload,
                          reinterpret_cast<const std::uint8_t*>(utf8_payload.data()),
                          utf8_payload.size());
    }
    if (!content_hash.empty()) {
        w.WriteStringField(proto::kCDFieldContentHash, content_hash);
    }
    return w.take();
}

std::vector<std::uint8_t> EncodeClipboardChangedText(std::string_view utf8_payload,
                                                     std::string_view content_hash,
                                                     ClockFn          clock) {
    auto   inner = EncodeClipboardDataInnerText(utf8_payload, content_hash);
    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::ClipboardChanged));
    w.WriteSubMessageField(proto::kFieldClipboardData, inner);
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

std::vector<std::uint8_t> EncodeDataRequest(std::string_view             content_hash,
                                            proto::ClipboardContentType content_type,
                                            ClockFn                      clock) {
    Writer inner;
    if (!content_hash.empty()) {
        inner.WriteStringField(proto::kReqFieldContentHash, content_hash);
    }
    inner.WriteEnumField(proto::kReqFieldContentType,
                         static_cast<std::int32_t>(content_type));

    Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(HelperMessageType::DataRequest));
    w.WriteSubMessageField(proto::kFieldDataRequest, inner.take());
    w.WriteUint64Field(proto::kFieldTimestamp, clock());
    return w.take();
}

// ─── Decoders ────────────────────────────────────────────────────────────

bool ParseHelperMessage(const std::vector<std::uint8_t>& in,
                        ParsedHelperMessage&             out) {
    out = {};
    Reader r(in);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kFieldType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.type = static_cast<HelperMessageType>(static_cast<std::int32_t>(v));
        } else if (field == proto::kFieldClipboardData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.clipboard_data_ptr, out.clipboard_data_len)) return false;
        } else if (field == proto::kFieldAnnouncement && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.announcement_ptr, out.announcement_len)) return false;
        } else if (field == proto::kFieldProvideData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.provide_data_ptr, out.provide_data_len)) return false;
        } else if (field == proto::kFieldFileChunkRequest && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.file_chunk_request_ptr, out.file_chunk_request_len)) return false;
        } else if (field == proto::kFieldFileChunkData && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.file_chunk_data_ptr, out.file_chunk_data_len)) return false;
        } else {
            // Unknown field — proto3 says skip and keep going.
            if (!r.SkipField(wire)) return false;
        }
    }
    return true;
}

bool ParseClipboardData(const std::uint8_t*  data,
                        std::size_t          len,
                        ParsedClipboardData& out) {
    out = {};
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kCDFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(static_cast<std::int32_t>(v));
        } else if (field == proto::kCDFieldPayload && wire == WireType::LengthDelim) {
            if (!r.ReadLengthDelim(out.payload_ptr, out.payload_len)) return false;
        } else if (field == proto::kCDFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
        } else if (field == proto::kCDFieldTransferId && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.transfer_id.assign(reinterpret_cast<const char*>(p), n);
        } else {
            if (!r.SkipField(wire)) return false;
        }
    }
    return true;
}

bool ParseAnnouncement(const std::uint8_t* data,
                       std::size_t         len,
                       ParsedAnnouncement& out) {
    out = {};
    Reader r(data, len);
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        if (!r.ReadTag(field, wire)) return false;
        if (field == proto::kAnnFieldContentType && wire == WireType::Varint) {
            std::uint64_t v = 0;
            if (!r.ReadVarint(v)) return false;
            out.content_type = static_cast<ClipboardContentType>(static_cast<std::int32_t>(v));
        } else if (field == proto::kAnnFieldContentHash && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.content_hash.assign(reinterpret_cast<const char*>(p), n);
        } else if (field == proto::kAnnFieldTransferId && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.transfer_id.assign(reinterpret_cast<const char*>(p), n);
        } else {
            if (!r.SkipField(wire)) return false;
        }
    }
    return true;
}

bool ParseProvideData(const std::uint8_t* data,
                      std::size_t         len,
                      ParsedProvideData&  out) {
    out = {};
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
        } else if (field == proto::kPDFieldData && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            if (!r.ReadLengthDelim(p, n)) return false;
            out.data.assign(p, p + n);
        } else {
            if (!r.SkipField(wire)) return false;
        }
    }
    return true;
}

}  // namespace leviathan::clipboard_helper::dispatch_codec
