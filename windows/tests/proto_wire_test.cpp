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
    w.WriteVarint(127);  // 0x7F — last single-byte varint
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x7F}));
}

TEST(ProtoWire, WriteVarintTwoByteBoundary) {
    Writer w;
    w.WriteVarint(128);  // first multi-byte varint
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x80, 0x01}));
}

TEST(ProtoWire, WriteVarintCanonical300) {
    // Canonical example from the protobuf spec docs.
    Writer w;
    w.WriteVarint(300);
    EXPECT_TRUE(BytesEqual(w.bytes(), {0xAC, 0x02}));
}

TEST(ProtoWire, WriteVarintLargeUint32) {
    Writer w;
    w.WriteVarint(0xFFFFFFFFu);  // 5-byte varint
    EXPECT_TRUE(BytesEqual(w.bytes(), {0xFF, 0xFF, 0xFF, 0xFF, 0x0F}));
}

TEST(ProtoWire, WriteVarintMaxUint64) {
    Writer w;
    w.WriteVarint(std::numeric_limits<std::uint64_t>::max());  // 10-byte varint
    EXPECT_EQ(w.bytes().size(), static_cast<std::size_t>(10));
    // The first 9 bytes are 0xFF (continuation set + 0x7F payload).
    for (std::size_t i = 0; i < 9; ++i) {
        EXPECT_EQ(static_cast<int>(w.bytes()[i]), 0xFF);
    }
    EXPECT_EQ(static_cast<int>(w.bytes()[9]), 0x01);
}

// ─── Tag encoding ─────────────────────────────────────────────────────────

TEST(ProtoWire, MakeTagPacksFieldAndWire) {
    EXPECT_EQ(MakeTag(1, WireType::Varint),     static_cast<std::uint32_t>(0x08));
    EXPECT_EQ(MakeTag(1, WireType::LengthDelim), static_cast<std::uint32_t>(0x0A));
    EXPECT_EQ(MakeTag(15, WireType::LengthDelim), static_cast<std::uint32_t>(0x7A));
    // Field 16 crosses the 1-byte varint boundary for the tag itself.
    EXPECT_EQ(MakeTag(16, WireType::LengthDelim), static_cast<std::uint32_t>(0x82));
}

TEST(ProtoWire, WriteTagEncodesAsVarint) {
    Writer w;
    w.WriteTag(16, WireType::LengthDelim);  // tag=130, varint=[0x82, 0x01]
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
    w.WriteUint32Field(7, 42);  // tag=(7<<3)|0=0x38, value=42=0x2A
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x38, 0x2A}));
}

TEST(ProtoWire, WriteInt32FieldEncodesNegativeAs10ByteVarint) {
    Writer w;
    w.WriteInt32Field(1, -1);
    // Tag (1, Varint) = 0x08, then ten bytes of 0xFF...01 for varint(uint64_max).
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
    w.WriteBoolField(5, true);  // tag=(5<<3)|0=0x28, payload=0x01
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x28, 0x01}));
}

TEST(ProtoWire, WriteStringFieldSuppressesEmpty) {
    Writer w;
    w.WriteStringField(2, "");
    EXPECT_TRUE(w.bytes().empty());
}

TEST(ProtoWire, WriteStringFieldEmitsTagLengthAndPayload) {
    Writer w;
    w.WriteStringField(1, "hi");  // tag=(1<<3)|2=0x0A, len=2, then 'h','i'
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
    w.WriteBytesField(3, data, 3);  // tag=(3<<3)|2=0x1A
    EXPECT_TRUE(BytesEqual(w.bytes(), {0x1A, 0x03, 0xDE, 0xAD, 0xBE}));
}

TEST(ProtoWire, WriteSubMessageFieldEmitsEvenWhenEmpty) {
    // Unlike scalars, sub-message fields don't get the proto3 zero-suppression
    // — the encoder always emits the tag + length prefix so a present-but-
    // empty sub-message stays observable on the wire.
    Writer w;
    const std::vector<std::uint8_t> sub;
    w.WriteSubMessageField(4, sub);  // tag=(4<<3)|2=0x22, len=0
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
    // 0x80 = continuation bit set, then EOF — varint is incomplete.
    const std::uint8_t bad[] = {0x80};
    Reader r(bad, sizeof(bad));
    std::uint64_t v = 0;
    EXPECT_FALSE(r.ReadVarint(v));
}

TEST(ProtoWire, ReaderRejectsOverlongVarint) {
    // Eleven continuation bytes — past the 10-byte ceiling for a uint64.
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
    // Tag (field=1, LengthDelim) + length=10 + only 3 body bytes available.
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
    // We never write Fixed32 / Fixed64 fields ourselves (the helper schema is
    // all varint + LengthDelim), but the skipper must still advance past them
    // so a future schema bump doesn't trap on an unknown tag.
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
    // StartGroup / EndGroup are proto2 leftovers we never expect to see.
    Reader r(nullptr, 0);
    EXPECT_FALSE(r.SkipField(WireType::StartGroup));
    EXPECT_FALSE(r.SkipField(WireType::EndGroup));
}

// ─── Roundtrip: writer then reader ────────────────────────────────────────

TEST(ProtoWire, RoundtripSimulatedHelperMessage) {
    // Mirror what dispatch.cpp's EncodeReady produces: type (enum, varint
    // in field 1) + timestamp (uint64 in field 7) + a string we tack on as
    // field 6 so we exercise length-delimited decode too.
    Writer w;
    w.WriteEnumField(1, 14);                     // HelperMessageType::Ready
    w.WriteStringField(6, "hello-world");        // error_message piggyback
    w.WriteUint64Field(7, 0x0102030405060708ull);

    Reader r(w.bytes());
    int           seen_type        = -1;
    std::string   seen_error_msg;
    std::uint64_t seen_timestamp   = 0;
    while (!r.eof()) {
        std::uint32_t field = 0;
        WireType      wire  = WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == 1 && wire == WireType::Varint) {
            std::uint64_t v = 0;
            ASSERT_TRUE(r.ReadVarint(v));
            seen_type = static_cast<int>(v);
        } else if (field == 6 && wire == WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            seen_error_msg.assign(reinterpret_cast<const char*>(p), n);
        } else if (field == 7 && wire == WireType::Varint) {
            ASSERT_TRUE(r.ReadVarint(seen_timestamp));
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_EQ(seen_type, 14);
    EXPECT_EQ(seen_error_msg, std::string("hello-world"));
    EXPECT_EQ(seen_timestamp, static_cast<std::uint64_t>(0x0102030405060708ull));
}

TEST(ProtoWire, RoundtripSubMessageDecodesIndependently) {
    // Build an inner ClipboardData-ish sub: content_type=1, payload="hi".
    Writer inner;
    inner.WriteEnumField(1, 1);
    inner.WriteBytesField(2,
        reinterpret_cast<const std::uint8_t*>("hi"), 2);
    const auto inner_bytes = inner.bytes();

    Writer outer;
    outer.WriteSubMessageField(2, inner_bytes);

    // Outer: field 2 LengthDelim, body == inner_bytes.
    Reader r(outer.bytes());
    std::uint32_t field = 0;
    WireType      wire  = WireType::Varint;
    ASSERT_TRUE(r.ReadTag(field, wire));
    ASSERT_EQ(field, static_cast<std::uint32_t>(2));
    ASSERT_EQ(static_cast<int>(wire), static_cast<int>(WireType::LengthDelim));

    const std::uint8_t* sub_ptr = nullptr;
    std::size_t         sub_len = 0;
    ASSERT_TRUE(r.ReadLengthDelim(sub_ptr, sub_len));
    ASSERT_EQ(sub_len, inner_bytes.size());

    // Decode the inner independently.
    Reader inner_r(sub_ptr, sub_len);
    int         ct = 0;
    std::string payload;
    while (!inner_r.eof()) {
        std::uint32_t f = 0;
        WireType      wt = WireType::Varint;
        ASSERT_TRUE(inner_r.ReadTag(f, wt));
        if (f == 1) {
            std::uint64_t v = 0;
            ASSERT_TRUE(inner_r.ReadVarint(v));
            ct = static_cast<int>(v);
        } else if (f == 2) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(inner_r.ReadLengthDelim(p, n));
            payload.assign(reinterpret_cast<const char*>(p), n);
        }
    }
    EXPECT_EQ(ct, 1);
    EXPECT_EQ(payload, std::string("hi"));
    EXPECT_TRUE(r.eof());
}

// ─── Reader position bookkeeping ──────────────────────────────────────────

TEST(ProtoWire, ReaderTracksPositionAndRemaining) {
    Writer w;
    w.WriteVarint(300);            // 2 bytes
    w.WriteVarint(1);              // 1 byte
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
