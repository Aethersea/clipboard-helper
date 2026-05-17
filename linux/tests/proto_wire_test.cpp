// proto_wire unit tests — copied verbatim from windows/tests/proto_wire_test.cpp.
//
// The proto_wire module is 100% portable C++17; the test surface stays
// identical across platforms so any drift in encoder behaviour shows up
// here before it can hit production.

#include "proto_wire.h"
#include "test_lite.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using leviathan::clipboard_helper::proto_wire::MakeTag;
using leviathan::clipboard_helper::proto_wire::Reader;
using leviathan::clipboard_helper::proto_wire::WireType;
using leviathan::clipboard_helper::proto_wire::Writer;

namespace {

bool BytesEqual(const std::vector<std::uint8_t>& got,
                std::initializer_list<int> expected) {
    if (got.size() != expected.size()) return false;
    auto it = expected.begin();
    for (std::size_t i = 0; i < got.size(); ++i, ++it) {
        if (got[i] != static_cast<std::uint8_t>(*it)) return false;
    }
    return true;
}

}  // namespace

// ─── Varint encoding ──────────────────────────────────────────────────────

TEST(ProtoWire, WriteVarintZero) {
    Writer w;
    w.WriteVarint(0);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x00}));
}

TEST(ProtoWire, WriteVarintSingleByteBoundary) {
    Writer w;
    w.WriteVarint(127);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x7F}));
}

TEST(ProtoWire, WriteVarintTwoByteBoundary) {
    Writer w;
    w.WriteVarint(128);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x80, 0x01}));
}

TEST(ProtoWire, WriteVarintCanonical300) {
    Writer w;
    w.WriteVarint(300);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0xAC, 0x02}));
}

TEST(ProtoWire, WriteVarintLargeUint32) {
    Writer w;
    w.WriteVarint(0xFFFFFFFFu);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0xFF, 0xFF, 0xFF, 0xFF, 0x0F}));
}

TEST(ProtoWire, WriteVarintMaxUint64) {
    Writer w;
    w.WriteVarint(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(w.bytes().size(), static_cast<std::size_t>(10));
    for (std::size_t i = 0; i < 9; ++i) {
        EXPECT_EQ(static_cast<int>(w.bytes()[i]), 0xFF);
    }
    EXPECT_EQ(static_cast<int>(w.bytes()[9]), 0x01);
}

// ─── Tag encoding ─────────────────────────────────────────────────────────

TEST(ProtoWire, MakeTagPacksFieldAndWire) {
    EXPECT_EQ(MakeTag(1, WireType::Varint),       static_cast<std::uint32_t>(0x08));
    EXPECT_EQ(MakeTag(1, WireType::LengthDelim),  static_cast<std::uint32_t>(0x0A));
    EXPECT_EQ(MakeTag(15, WireType::LengthDelim), static_cast<std::uint32_t>(0x7A));
    EXPECT_EQ(MakeTag(16, WireType::LengthDelim), static_cast<std::uint32_t>(0x82));
}

TEST(ProtoWire, WriteTagEncodesAsVarint) {
    Writer w;
    w.WriteTag(16, WireType::LengthDelim);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x82, 0x01}));
}

// ─── Proto3 zero-suppression ──────────────────────────────────────────────

TEST(ProtoWire, WriteUint32FieldSuppressesZero) {
    Writer w;
    w.WriteUint32Field(7, 0);
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ProtoWire, WriteUint32FieldEmitsNonZero) {
    Writer w;
    w.WriteUint32Field(7, 42);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x38, 0x2A}));
}

TEST(ProtoWire, WriteInt32FieldEncodesNegativeAs10ByteVarint) {
    Writer w;
    w.WriteInt32Field(1, -1);
    ASSERT_EQ(w.bytes().size(), static_cast<std::size_t>(11));
    EXPECT_EQ(static_cast<int>(w.bytes()[0]), 0x08);
    for (std::size_t i = 1; i < 10; ++i) {
        EXPECT_EQ(static_cast<int>(w.bytes()[i]), 0xFF);
    }
    EXPECT_EQ(static_cast<int>(w.bytes()[10]), 0x01);
}

TEST(ProtoWire, WriteBoolFieldSuppressesFalse) {
    Writer w;
    w.WriteBoolField(5, false);
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ProtoWire, WriteBoolFieldEmitsTrue) {
    Writer w;
    w.WriteBoolField(5, true);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x28, 0x01}));
}

TEST(ProtoWire, WriteStringFieldSuppressesEmpty) {
    Writer w;
    w.WriteStringField(2, "");
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ProtoWire, WriteStringFieldEmitsTagLengthAndPayload) {
    Writer w;
    w.WriteStringField(1, "hi");
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x0A, 0x02, 'h', 'i'}));
}

TEST(ProtoWire, WriteBytesFieldSuppressesEmpty) {
    Writer w;
    const std::uint8_t empty_data[1] = {0};
    w.WriteBytesField(2, empty_data, 0);
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ProtoWire, WriteBytesFieldEmitsTagLengthAndPayload) {
    Writer w;
    const std::uint8_t data[3] = {0xDE, 0xAD, 0xBE};
    w.WriteBytesField(3, data, 3);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x1A, 0x03, 0xDE, 0xAD, 0xBE}));
}

TEST(ProtoWire, WriteSubMessageFieldEmitsEvenWhenEmpty) {
    Writer w;
    const std::vector<std::uint8_t> sub;
    w.WriteSubMessageField(4, sub);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x22, 0x00}));
}

// ─── Reader: varint decoding ──────────────────────────────────────────────

TEST(ProtoWire, ReaderRoundTripsCanonical300) {
    Writer w;
    w.WriteVarint(300);
    Reader r(w.bytes());
    std::uint64_t v = 0;
    ASSERT_TRUE(r.ReadVarint(v));
    EXPECT_EQ(v, static_cast<std::uint64_t>(300));
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, ReaderRejectsTruncatedVarint) {
    const std::uint8_t bad[] = {0x80};
    Reader r(bad, sizeof(bad));
    std::uint64_t v = 0;
    EXPECT_FALSE(r.ReadVarint(v));
}

TEST(ProtoWire, ReaderRejectsOverlongVarint) {
    const std::uint8_t overlong[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                                     0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    Reader r(overlong, sizeof(overlong));
    std::uint64_t v = 0;
    EXPECT_FALSE(r.ReadVarint(v));
}

TEST(ProtoWire, ReadTagSplitsFieldAndWire) {
    Writer w;
    w.WriteTag(16, WireType::LengthDelim);
    Reader r(w.bytes());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    EXPECT_EQ(field, static_cast<std::uint32_t>(16));
    EXPECT_EQ(static_cast<int>(wire), static_cast<int>(WireType::LengthDelim));
    EXPECT_TRUE(r.eof());
}

// ─── Reader: length-delimited fields ──────────────────────────────────────

TEST(ProtoWire, ReadLengthDelimReturnsPointerAndLength) {
    Writer w;
    const std::uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    w.WriteBytesField(1, payload, sizeof(payload));

    Reader r(w.bytes());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_EQ(field, static_cast<std::uint32_t>(1));
    ASSERT_EQ(static_cast<int>(wire), static_cast<int>(WireType::LengthDelim));

    const std::uint8_t* p = nullptr;
    std::size_t         n = 0;
    ASSERT_TRUE(r.ReadLengthDelim(p, n));
    ASSERT_EQ(n, sizeof(payload));
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(std::memcmp(p, payload, n), 0);
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, ReadLengthDelimRejectsLengthOverflow) {
    const std::uint8_t bad[] = {0x0A, 0x0A, 0x01, 0x02, 0x03};
    Reader r(bad, sizeof(bad));
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    const std::uint8_t* p = nullptr;
    std::size_t         n = 0;
    EXPECT_FALSE(r.ReadLengthDelim(p, n));
}

// ─── Reader: skip unknown fields ──────────────────────────────────────────

TEST(ProtoWire, SkipFieldVarintConsumesValue) {
    Writer w;
    w.WriteVarint(MakeTag(99, WireType::Varint));
    w.WriteVarint(300);
    w.WriteVarint(MakeTag(1, WireType::Varint));
    w.WriteVarint(42);

    Reader r(w.bytes());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_EQ(field, static_cast<std::uint32_t>(99));
    ASSERT_TRUE(r.SkipField(wire));

    ASSERT_TRUE(r.ReadTag(field, wire));
    EXPECT_EQ(field, static_cast<std::uint32_t>(1));
    std::uint64_t v = 0;
    ASSERT_TRUE(r.ReadVarint(v));
    EXPECT_EQ(v, static_cast<std::uint64_t>(42));
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, SkipFieldLengthDelimConsumesPayload) {
    Writer w;
    w.WriteStringField(99, "ignore-me");
    w.WriteUint32Field(1, 7);

    Reader r(w.bytes());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_EQ(field, static_cast<std::uint32_t>(99));
    ASSERT_TRUE(r.SkipField(wire));

    ASSERT_TRUE(r.ReadTag(field, wire));
    EXPECT_EQ(field, static_cast<std::uint32_t>(1));
    std::uint64_t v = 0;
    ASSERT_TRUE(r.ReadVarint(v));
    EXPECT_EQ(v, static_cast<std::uint64_t>(7));
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, SkipFieldFixed32And64) {
    std::vector<std::uint8_t> buf;
    buf.push_back(static_cast<std::uint8_t>(MakeTag(1, WireType::Fixed32)));
    for (int i = 0; i < 4; ++i) buf.push_back(0xAA);
    buf.push_back(static_cast<std::uint8_t>(MakeTag(2, WireType::Fixed64)));
    for (int i = 0; i < 8; ++i) buf.push_back(0xBB);

    Reader r(buf.data(), buf.size());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;

    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_TRUE(r.SkipField(wire));
    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_TRUE(r.SkipField(wire));
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, SkipFieldRejectsGroupWireTypes) {
    Reader r(nullptr, 0);
    EXPECT_FALSE(r.SkipField(WireType::StartGroup));
    EXPECT_FALSE(r.SkipField(WireType::EndGroup));
}

// ─── Reader position bookkeeping ──────────────────────────────────────────

TEST(ProtoWire, ReaderTracksPositionAndRemaining) {
    Writer w;
    w.WriteVarint(300);
    w.WriteVarint(1);
    const auto bytes = w.bytes();
    ASSERT_EQ(bytes.size(), static_cast<std::size_t>(3));

    Reader r(bytes);
    EXPECT_EQ(r.pos(), static_cast<std::size_t>(0));
    EXPECT_EQ(r.remaining(), static_cast<std::size_t>(3));

    std::uint64_t v = 0;
    ASSERT_TRUE(r.ReadVarint(v));
    EXPECT_EQ(r.pos(), static_cast<std::size_t>(2));
    EXPECT_EQ(r.remaining(), static_cast<std::size_t>(1));
    EXPECT_FALSE(r.eof());

    ASSERT_TRUE(r.ReadVarint(v));
    EXPECT_EQ(r.pos(), static_cast<std::size_t>(3));
    EXPECT_EQ(r.remaining(), static_cast<std::size_t>(0));
    EXPECT_TRUE(r.eof());
}

TEST(ProtoWire, ReaderRejectsReadPastEndOfBuffer) {
    Reader r(nullptr, 0);
    std::uint64_t v = 0;
    EXPECT_FALSE(r.ReadVarint(v));
    std::uint32_t f = 0;
    WireType      wt = WireType::Varint;
    EXPECT_FALSE(r.ReadTag(f, wt));
}
