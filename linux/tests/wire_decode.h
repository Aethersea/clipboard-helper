#pragma once
//
// wire_decode — independent, test-only decoders for the HelperMessage
// frames the helper emits. They deliberately use only proto_wire::Reader
// plus the field numbers from Proto/*.proto (hard-coded here, NOT taken
// from helper_proto.h) so a wrong constant in production code cannot
// cancel itself out in the tests.

#include <cstdint>
#include <string>
#include <vector>

#include "proto_wire.h"

namespace wiretest {

namespace pw = leviathan::clipboard_helper::proto_wire;

struct File {
    std::string   file_id, filename, relative_path, mime_type, checksum;
    std::uint64_t file_size{0};
    bool          is_directory{false};
    bool          has_file_size{false};
    bool          has_is_directory{false};
};

struct Frame {
    bool          ok{false};
    int           type{0};
    std::uint64_t timestamp{0};
    std::string   error_message;

    // ClipboardData (field 2)
    bool                      has_clipboard_data{false};
    int                       content_type{0};
    std::string               payload;
    std::string               content_hash;
    std::string               cd_transfer_id;
    std::vector<File>         files;

    // HelperFileChunkData (field 9)
    bool          has_chunk{false};
    std::string   transfer_id, file_id, data, error;
    std::uint64_t offset{0};
    bool          is_last{false};

    // HelperFileChunkRequest (field 8) – only used to build, not decoded.
};

inline bool ReadStr(pw::Reader& r, std::string& out) {
    const std::uint8_t* p = nullptr;
    std::size_t         n = 0;
    if (!r.ReadLengthDelim(p, n)) return false;
    out.assign(reinterpret_cast<const char*>(p), n);
    return true;
}

inline bool DecodeFile(const std::uint8_t* p, std::size_t n, File& f) {
    pw::Reader r(p, n);
    while (!r.eof()) {
        std::uint32_t fld;
        pw::WireType  wt;
        if (!r.ReadTag(fld, wt)) return false;
        if (wt == pw::Varint) {
            std::uint64_t v;
            if (!r.ReadVarint(v)) return false;
            if (fld == 4) { f.file_size = v; f.has_file_size = true; }
            else if (fld == 7) { f.is_directory = v != 0; f.has_is_directory = true; }
        } else if (wt == pw::LengthDelim) {
            std::string s;
            if (!ReadStr(r, s)) return false;
            if (fld == 1) f.file_id = s;
            else if (fld == 2) f.filename = s;
            else if (fld == 3) f.relative_path = s;
            else if (fld == 5) f.mime_type = s;
            else if (fld == 6) f.checksum = s;
        } else if (!r.SkipField(wt)) {
            return false;
        }
    }
    return true;
}

inline bool DecodeClipboardData(const std::uint8_t* p, std::size_t n, Frame& out) {
    pw::Reader r(p, n);
    while (!r.eof()) {
        std::uint32_t fld;
        pw::WireType  wt;
        if (!r.ReadTag(fld, wt)) return false;
        if (wt == pw::Varint) {
            std::uint64_t v;
            if (!r.ReadVarint(v)) return false;
            if (fld == 1) out.content_type = static_cast<int>(v);
        } else if (wt == pw::LengthDelim) {
            const std::uint8_t* sp;
            std::size_t         sn;
            if (!r.ReadLengthDelim(sp, sn)) return false;
            if (fld == 2) out.payload.assign(reinterpret_cast<const char*>(sp), sn);
            else if (fld == 3) out.content_hash.assign(reinterpret_cast<const char*>(sp), sn);
            else if (fld == 5) out.cd_transfer_id.assign(reinterpret_cast<const char*>(sp), sn);
            else if (fld == 4) {
                File f;
                if (!DecodeFile(sp, sn, f)) return false;
                out.files.push_back(std::move(f));
            }
        } else if (!r.SkipField(wt)) {
            return false;
        }
    }
    return true;
}

inline bool DecodeChunk(const std::uint8_t* p, std::size_t n, Frame& out) {
    pw::Reader r(p, n);
    while (!r.eof()) {
        std::uint32_t fld;
        pw::WireType  wt;
        if (!r.ReadTag(fld, wt)) return false;
        if (wt == pw::Varint) {
            std::uint64_t v;
            if (!r.ReadVarint(v)) return false;
            if (fld == 3) out.offset = v;
            else if (fld == 5) out.is_last = v != 0;
        } else if (wt == pw::LengthDelim) {
            std::string s;
            if (!ReadStr(r, s)) return false;
            if (fld == 1) out.transfer_id = s;
            else if (fld == 2) out.file_id = s;
            else if (fld == 4) out.data = s;
            else if (fld == 6) out.error = s;
        } else if (!r.SkipField(wt)) {
            return false;
        }
    }
    return true;
}

// Decode a HelperMessage envelope (Proto/clipboard_helper.proto).
inline Frame Decode(const std::vector<std::uint8_t>& bytes) {
    Frame f;
    pw::Reader r(bytes);
    while (!r.eof()) {
        std::uint32_t fld;
        pw::WireType  wt;
        if (!r.ReadTag(fld, wt)) return f;
        if (wt == pw::Varint) {
            std::uint64_t v;
            if (!r.ReadVarint(v)) return f;
            if (fld == 1) f.type = static_cast<int>(v);
            else if (fld == 7) f.timestamp = v;
        } else if (wt == pw::LengthDelim) {
            const std::uint8_t* sp;
            std::size_t         sn;
            if (!r.ReadLengthDelim(sp, sn)) return f;
            if (fld == 2) {
                f.has_clipboard_data = true;
                if (!DecodeClipboardData(sp, sn, f)) return f;
            } else if (fld == 6) {
                f.error_message.assign(reinterpret_cast<const char*>(sp), sn);
            } else if (fld == 9) {
                f.has_chunk = true;
                if (!DecodeChunk(sp, sn, f)) return f;
            }
        } else if (!r.SkipField(wt)) {
            return f;
        }
    }
    f.ok = true;
    return f;
}

// Message-type numbers from Proto/clipboard_helper.proto.
constexpr int kSetClipboard = 1, kChanged = 10, kDataRequest = 11, kError = 13, kReady = 14,
              kChunkRequest = 15, kChunkData = 16, kProgress = 17;
constexpr int kText = 1, kFiles = 3;

// ─── Builders for inbound frames ─────────────────────────────────────────

inline std::vector<std::uint8_t> Envelope(int type, std::uint32_t sub_field = 0,
                                          const std::vector<std::uint8_t>* sub = nullptr) {
    pw::Writer w;
    w.WriteEnumField(1, type);
    if (sub != nullptr) w.WriteSubMessageField(sub_field, *sub);
    return w.take();
}

inline std::vector<std::uint8_t> ChunkRequest(const std::string& transfer_id,
                                              const std::string& file_id,
                                              std::uint64_t offset, std::uint32_t size) {
    pw::Writer w;
    if (!transfer_id.empty()) w.WriteStringField(1, transfer_id);
    if (!file_id.empty()) w.WriteStringField(2, file_id);
    if (offset != 0) w.WriteUint64Field(3, offset);
    if (size != 0) w.WriteUint32Field(4, size);
    const auto sub = w.take();
    return Envelope(kChunkRequest, 8, &sub);
}

inline std::vector<std::uint8_t> SetClipboardText(const std::string& text) {
    pw::Writer w;
    w.WriteEnumField(1, kText);
    w.WriteBytesField(2, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    const auto sub = w.take();
    return Envelope(kSetClipboard, 2, &sub);
}

}  // namespace wiretest
