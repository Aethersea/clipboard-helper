// dispatch_codec unit tests — covers HelperMessage encode/parse round
// trips for every message type the P1 helper handles. Production code
// in dispatch.cpp depends on these encoders producing wire-format the
// parents (shen Rust, leviathan Go) can decode; round-tripping through
// the helper's own parser checks at least that the encoders are
// self-consistent. Cross-language compatibility is verified separately
// by the parent's protobuf decoder during integration testing.

#include "dispatch_codec.h"
#include "helper_proto.h"
#include "test_lite.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ch = leviathan::clipboard_helper;
namespace cc = leviathan::clipboard_helper::dispatch_codec;

namespace {

// Deterministic clock for encoder timestamp assertions.
std::uint64_t StubClock() { return 0x0102030405060708ull; }

}  // namespace

// ─── EncodeReady ─────────────────────────────────────────────────────────

TEST(DispatchCodec, EncodeReadyEmitsCorrectType) {
    auto frame = cc::EncodeReady(StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::Ready));
}

// ─── EncodeError ─────────────────────────────────────────────────────────

TEST(DispatchCodec, EncodeErrorEmitsMessageBytes) {
    auto frame = cc::EncodeError("oops", StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::Error));
    // The error_message field is the string at field 6 — find it in the
    // raw frame to confirm it's present (cheap byte search; the wire
    // layout is `... 0x32 <len> "oops" ...`).
    const std::string needle = "oops";
    bool found = false;
    for (std::size_t i = 0; i + needle.size() <= frame.size(); ++i) {
        if (std::string(reinterpret_cast<const char*>(&frame[i]), needle.size())
            == needle) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(DispatchCodec, EncodeErrorEmptyMessageStillProducesValidFrame) {
    auto frame = cc::EncodeError("", StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::Error));
}

// ─── EncodeClipboardChangedText / ParseClipboardData ─────────────────────

TEST(DispatchCodec, EncodeClipboardChangedTextRoundTrips) {
    auto frame = cc::EncodeClipboardChangedText(
        "hello world", "abc123hash", StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::ClipboardChanged));
    ASSERT_TRUE(parsed.clipboard_data_ptr != nullptr);

    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(parsed.clipboard_data_ptr,
                                       parsed.clipboard_data_len, cd));
    EXPECT_EQ(static_cast<int>(cd.content_type),
              static_cast<int>(ch::proto::ClipboardContentType::Text));
    const std::string payload(reinterpret_cast<const char*>(cd.payload_ptr),
                              cd.payload_len);
    EXPECT_EQ(payload, std::string("hello world"));
    EXPECT_EQ(cd.content_hash, std::string("abc123hash"));
}

TEST(DispatchCodec, EncodeClipboardChangedTextHandlesEmptyHash) {
    // Helper omits hash when it doesn't compute one; parent is expected
    // to compute its own from the payload.
    auto frame = cc::EncodeClipboardChangedText("payload", "", StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(parsed.clipboard_data_ptr,
                                       parsed.clipboard_data_len, cd));
    EXPECT_TRUE(cd.content_hash.empty());
}

TEST(DispatchCodec, EncodeClipboardChangedTextHandlesEmptyPayload) {
    auto frame = cc::EncodeClipboardChangedText("", "hash-only", StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(parsed.clipboard_data_ptr,
                                       parsed.clipboard_data_len, cd));
    EXPECT_EQ(cd.payload_len, static_cast<std::size_t>(0));
    EXPECT_EQ(cd.content_hash, std::string("hash-only"));
}

// ─── EncodeDataRequest ───────────────────────────────────────────────────

TEST(DispatchCodec, EncodeDataRequestRoundTripsHashAndType) {
    auto frame = cc::EncodeDataRequest(
        "deadbeef", ch::proto::ClipboardContentType::Text, StubClock);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::DataRequest));
    // The data_request sub-message lives at field 5; ParseHelperMessage
    // exposes its raw bytes — find the content_hash string within for
    // confirmation. (We could carve out a Parse(DataRequest) helper,
    // but for the P1 surface this is enough.)
    const std::string needle = "deadbeef";
    bool found = false;
    for (std::size_t i = 0; i + needle.size() <= frame.size(); ++i) {
        if (std::string(reinterpret_cast<const char*>(&frame[i]), needle.size())
            == needle) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ─── ParseHelperMessage envelope handling ─────────────────────────────────

TEST(DispatchCodec, ParseHelperMessageHandlesEmptyFrame) {
    // Empty input is a degenerate but valid HelperMessage (everything
    // defaults to zero). Parser must not crash.
    std::vector<std::uint8_t> empty;
    cc::ParsedHelperMessage parsed{};
    EXPECT_TRUE(cc::ParseHelperMessage(empty, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::Unspecified));
}

TEST(DispatchCodec, ParseHelperMessageRejectsMalformedTag) {
    // Bare continuation byte — invalid varint, no body. Parser bails.
    std::vector<std::uint8_t> bad = {0x80};
    cc::ParsedHelperMessage parsed{};
    EXPECT_FALSE(cc::ParseHelperMessage(bad, parsed));
}

TEST(DispatchCodec, ParseHelperMessageSkipsUnknownFields) {
    // Field 99 (varint) followed by field 1 (type=Ready). Parser must
    // skip past the unknown field and still find the type.
    std::vector<std::uint8_t> wire;
    // field=99 wire=Varint → tag = (99<<3)|0 = 792 = 0x98, 0x06
    wire.push_back(0x98); wire.push_back(0x06);
    wire.push_back(0x2A);  // value 42
    // field=1 wire=Varint → tag = 0x08; value=14 (Ready)
    wire.push_back(0x08); wire.push_back(0x0E);
    cc::ParsedHelperMessage parsed{};
    ASSERT_TRUE(cc::ParseHelperMessage(wire, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(ch::proto::HelperMessageType::Ready));
}

// ─── ParseAnnouncement / ParseProvideData ─────────────────────────────────

TEST(DispatchCodec, ParseAnnouncementExtractsContentTypeAndHash) {
    // Build a minimal ClipboardAnnouncement inline using proto_wire.
    // content_type=1 (Text), content_hash="hash-12"
    std::vector<std::uint8_t> ann = {
        0x08, 0x01,                          // field 1 varint = 1
        0x12, 0x07, 'h','a','s','h','-','1','2', // field 2 string len=7
    };
    cc::ParsedAnnouncement out{};
    ASSERT_TRUE(cc::ParseAnnouncement(ann.data(), ann.size(), out));
    EXPECT_EQ(static_cast<int>(out.content_type),
              static_cast<int>(ch::proto::ClipboardContentType::Text));
    EXPECT_EQ(out.content_hash, std::string("hash-12"));
}

TEST(DispatchCodec, ParseProvideDataExtractsHashAndPayload) {
    // content_hash="h" (field 1), data="ab" (field 2)
    std::vector<std::uint8_t> pd = {
        0x0A, 0x01, 'h',                      // field 1 string len=1
        0x12, 0x02, 'a', 'b',                 // field 2 bytes  len=2
    };
    cc::ParsedProvideData out{};
    ASSERT_TRUE(cc::ParseProvideData(pd.data(), pd.size(), out));
    EXPECT_EQ(out.content_hash, std::string("h"));
    ASSERT_EQ(out.data.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<int>(out.data[0]), static_cast<int>('a'));
    EXPECT_EQ(static_cast<int>(out.data[1]), static_cast<int>('b'));
}

TEST(DispatchCodec, ParseProvideDataHandlesEmptyData) {
    // Empty bytes are proto3-suppressed on encode, so the wire side
    // could be missing the data field entirely. content_hash present,
    // data absent.
    std::vector<std::uint8_t> pd = {0x0A, 0x04, 'a', 'b', 'c', 'd'};
    cc::ParsedProvideData out{};
    ASSERT_TRUE(cc::ParseProvideData(pd.data(), pd.size(), out));
    EXPECT_EQ(out.content_hash, std::string("abcd"));
    EXPECT_TRUE(out.data.empty());
}
