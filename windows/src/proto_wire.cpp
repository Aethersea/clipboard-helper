#include "proto_wire.h"

#include <cstring>

namespace leviathan::clipboard_helper::proto_wire {

// ─── Writer ────────────────────────────────────────────────────────────────

void Writer::WriteVarint(std::uint64_t value) {
    while (value >= 0x80) {
        buf_.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    buf_.push_back(static_cast<std::uint8_t>(value));
}

void Writer::WriteTag(std::uint32_t field_number, WireType wire) {
    WriteVarint(MakeTag(field_number, wire));
}

void Writer::WriteUint32Field(std::uint32_t field_number, std::uint32_t value) {
    if (value == 0) return;  // proto3 default-suppression
    WriteTag(field_number, WireType::Varint);
    WriteVarint(value);
}

void Writer::WriteInt32Field(std::uint32_t field_number, std::int32_t value) {
    if (value == 0) return;
    WriteTag(field_number, WireType::Varint);
    // Negative int32 is encoded as 10-byte varint per proto wire spec.
    WriteVarint(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void Writer::WriteUint64Field(std::uint32_t field_number, std::uint64_t value) {
    if (value == 0) return;
    WriteTag(field_number, WireType::Varint);
    WriteVarint(value);
}

void Writer::WriteBoolField(std::uint32_t field_number, bool value) {
    if (!value) return;
    WriteTag(field_number, WireType::Varint);
    WriteVarint(value ? 1 : 0);
}

void Writer::WriteEnumField(std::uint32_t field_number, std::int32_t value) {
    WriteInt32Field(field_number, value);
}

void Writer::WriteStringField(std::uint32_t field_number, std::string_view value) {
    if (value.empty()) return;
    WriteTag(field_number, WireType::LengthDelim);
    WriteVarint(static_cast<std::uint64_t>(value.size()));
    buf_.insert(buf_.end(),
                reinterpret_cast<const std::uint8_t*>(value.data()),
                reinterpret_cast<const std::uint8_t*>(value.data()) + value.size());
}

void Writer::WriteBytesField(std::uint32_t field_number, const std::uint8_t* data, std::size_t len) {
    if (len == 0) return;
    WriteTag(field_number, WireType::LengthDelim);
    WriteVarint(static_cast<std::uint64_t>(len));
    buf_.insert(buf_.end(), data, data + len);
}

void Writer::WriteSubMessageField(std::uint32_t field_number, const std::vector<std::uint8_t>& sub) {
    WriteTag(field_number, WireType::LengthDelim);
    WriteVarint(static_cast<std::uint64_t>(sub.size()));
    buf_.insert(buf_.end(), sub.begin(), sub.end());
}

// ─── Reader ────────────────────────────────────────────────────────────────

bool Reader::ReadVarint(std::uint64_t& out_value) {
    out_value = 0;
    int shift = 0;
    while (true) {
        if (pos_ >= size_) return false;
        const auto b = data_[pos_++];
        out_value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;  // overflow
    }
}

bool Reader::ReadTag(std::uint32_t& out_field, WireType& out_wire) {
    std::uint64_t tag = 0;
    if (!ReadVarint(tag)) return false;
    out_field = static_cast<std::uint32_t>(tag >> 3);
    out_wire  = static_cast<WireType>(tag & 0x07);
    return true;
}

bool Reader::ReadLengthDelim(const std::uint8_t*& out_data, std::size_t& out_len) {
    std::uint64_t len = 0;
    if (!ReadVarint(len)) return false;
    if (len > remaining()) return false;
    out_data = data_ + pos_;
    out_len  = static_cast<std::size_t>(len);
    pos_    += out_len;
    return true;
}

bool Reader::SkipField(WireType wire) {
    switch (wire) {
        case WireType::Varint: {
            std::uint64_t dummy = 0;
            return ReadVarint(dummy);
        }
        case WireType::Fixed64:
            if (remaining() < 8) return false;
            pos_ += 8;
            return true;
        case WireType::LengthDelim: {
            const std::uint8_t* d = nullptr;
            std::size_t         n = 0;
            return ReadLengthDelim(d, n);
        }
        case WireType::Fixed32:
            if (remaining() < 4) return false;
            pos_ += 4;
            return true;
        default:
            return false;
    }
}

}  // namespace leviathan::clipboard_helper::proto_wire
