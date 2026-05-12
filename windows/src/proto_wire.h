#pragma once
//
// proto_wire — minimal protobuf wire-format primitives.
//
// We intentionally avoid linking google::protobuf in the Windows helper:
//   - vcpkg's protobuf port requires `vcvarsall.bat`, which VS 2026 Build
//     Tools no longer ships (only VsDevCmd.bat). That makes a clean dev
//     environment significantly harder to bootstrap.
//   - The helper only needs to encode/decode a handful of `HelperMessage`
//     types with simple fields (varint enums, strings, bytes, repeated
//     nested messages). The runtime overhead of full libprotobuf is wasted.
//
// The encoder/decoder here covers wire types 0 (Varint) and 2 (LengthDelim),
// which are enough for every field used by `clipboard.proto` /
// `clipboard_helper.proto`. Float/double/fixed32/fixed64 are NOT supported —
// add them if a future schema needs them.
//
// Wire format reference: https://protobuf.dev/programming-guides/encoding/

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace leviathan::clipboard_helper::proto_wire {

enum WireType : std::uint8_t {
    Varint     = 0,
    Fixed64    = 1,
    LengthDelim = 2,
    StartGroup = 3,
    EndGroup   = 4,
    Fixed32    = 5,
};

constexpr std::uint32_t MakeTag(std::uint32_t field_number, WireType wire) {
    return (field_number << 3) | static_cast<std::uint32_t>(wire);
}

// ─── Writer ────────────────────────────────────────────────────────────────

class Writer {
public:
    void WriteVarint(std::uint64_t value);
    void WriteTag(std::uint32_t field_number, WireType wire);
    void WriteUint32Field(std::uint32_t field_number, std::uint32_t value);
    void WriteInt32Field(std::uint32_t field_number, std::int32_t value);
    void WriteUint64Field(std::uint32_t field_number, std::uint64_t value);
    void WriteBoolField(std::uint32_t field_number, bool value);
    void WriteEnumField(std::uint32_t field_number, std::int32_t value);
    void WriteStringField(std::uint32_t field_number, std::string_view value);
    void WriteBytesField(std::uint32_t field_number, const std::uint8_t* data, std::size_t len);

    // For nested messages: append a length-delimited sub-payload. The caller
    // owns serialization of the sub-message.
    void WriteSubMessageField(std::uint32_t field_number, const std::vector<std::uint8_t>& sub);

    const std::vector<std::uint8_t>& bytes() const { return buf_; }
    std::vector<std::uint8_t> take() { return std::move(buf_); }

private:
    std::vector<std::uint8_t> buf_;
};

// ─── Reader ────────────────────────────────────────────────────────────────

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit Reader(const std::vector<std::uint8_t>& v) : data_(v.data()), size_(v.size()) {}

    bool eof() const { return pos_ >= size_; }
    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return size_ - pos_; }

    // Read one tag (field_number + wire_type). Returns false on EOF or malformed.
    bool ReadTag(std::uint32_t& out_field, WireType& out_wire);

    bool ReadVarint(std::uint64_t& out_value);

    // Read a length-delimited payload (string/bytes/sub-message body).
    bool ReadLengthDelim(const std::uint8_t*& out_data, std::size_t& out_len);

    // Skip the payload for an unknown field given its wire type.
    bool SkipField(WireType wire);

private:
    const std::uint8_t* data_;
    std::size_t         size_;
    std::size_t         pos_{0};
};

}  // namespace leviathan::clipboard_helper::proto_wire
