// dispatch_codec — protobuf encode/decode tests for the HelperMessage
// schema. These exercise dispatch_codec.{h,cpp} which was split out of
// dispatch.cpp so the wire-format layer is independently testable
// without pulling in OLE / WIC / STA.

#include "dispatch_codec.h"
#include "helper_proto.h"
#include "proto_wire.h"
#include "test_lite.h"
#include "virtual_file_spec.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using leviathan::clipboard_helper::ClipboardSnapshot;
using leviathan::clipboard_helper::VirtualFileEntry;
using leviathan::clipboard_helper::VirtualFileSpec;
namespace codec = leviathan::clipboard_helper::dispatch_codec;
namespace proto = leviathan::clipboard_helper::proto;
namespace pw    = leviathan::clipboard_helper::proto_wire;

namespace {

// Deterministic clock stub so HelperMessage.timestamp asserts are stable.
constexpr std::uint64_t kStubTimestamp = 0x0123456789ABCDEFull;
std::uint64_t StubClock() { return kStubTimestamp; }

}  // namespace

// ─── EncodeReady / EncodeError ────────────────────────────────────────────

TEST(DispatchCodec, EncodeReadyEmbedsTypeAndClock) {
    const auto frame = codec::EncodeReady(&StubClock);
    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::Ready));
    // ParseHelperMessage doesn't extract timestamp; verify by walking the
    // frame directly so the test still locks the clock injection.
    pw::Reader r(frame);
    bool saw_ts = false;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == proto::kFieldTimestamp && wire == pw::WireType::Varint) {
            std::uint64_t v = 0;
            ASSERT_TRUE(r.ReadVarint(v));
            EXPECT_EQ(v, kStubTimestamp);
            saw_ts = true;
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_TRUE(saw_ts);
}

TEST(DispatchCodec, EncodeErrorOmitsEmptyMessage) {
    // proto3 zero-suppression: an empty error_message string is suppressed
    // off the wire so the parent sees the absence rather than a zero-len
    // bytes payload.
    const auto frame = codec::EncodeError("", &StubClock);
    pw::Reader r(frame);
    bool saw_error_field = false;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == proto::kFieldErrorMessage) saw_error_field = true;
        ASSERT_TRUE(r.SkipField(wire));
    }
    EXPECT_FALSE(saw_error_field);
}

TEST(DispatchCodec, EncodeErrorEmitsMessageWhenNonEmpty) {
    const auto frame = codec::EncodeError("boom", &StubClock);
    pw::Reader r(frame);
    std::string seen;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == proto::kFieldErrorMessage && wire == pw::WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            seen.assign(reinterpret_cast<const char*>(p), n);
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_EQ(seen, std::string("boom"));
}

// ─── ClipboardData encoders ───────────────────────────────────────────────

TEST(DispatchCodec, EncodeClipboardDataTextEncodesUtf8Payload) {
    ClipboardSnapshot snap;
    snap.content_type = proto::ClipboardContentType::Text;
    snap.text         = L"hi";  // ASCII; survives Wide → UTF-8 unchanged.

    const auto inner = codec::EncodeClipboardData(snap);
    codec::ParsedClipboardData parsed;
    ASSERT_TRUE(codec::ParseClipboardData(inner.data(), inner.size(), parsed));
    EXPECT_EQ(static_cast<int>(parsed.content_type),
              static_cast<int>(proto::ClipboardContentType::Text));
    ASSERT_EQ(parsed.payload_len, static_cast<std::size_t>(2));
    EXPECT_EQ(parsed.payload_ptr[0], static_cast<std::uint8_t>('h'));
    EXPECT_EQ(parsed.payload_ptr[1], static_cast<std::uint8_t>('i'));
}

TEST(DispatchCodec, EncodeClipboardDataImageBypassesUtf8Conversion) {
    // Image payload is raw bytes (PNG, typically). Make sure no UTF-8
    // re-encoding step touches the bytes.
    ClipboardSnapshot snap;
    snap.content_type = proto::ClipboardContentType::Image;
    snap.image_png    = {0x89, 'P', 'N', 'G', 0x00, 0xFF};

    const auto inner = codec::EncodeClipboardData(snap);
    codec::ParsedClipboardData parsed;
    ASSERT_TRUE(codec::ParseClipboardData(inner.data(), inner.size(), parsed));
    ASSERT_EQ(parsed.payload_len, snap.image_png.size());
    EXPECT_EQ(std::memcmp(parsed.payload_ptr, snap.image_png.data(),
                          snap.image_png.size()), 0);
}

TEST(DispatchCodec, EncodeClipboardDataFilesUsesPhysicalAndVirtualEntries) {
    ClipboardSnapshot snap;
    snap.content_type = proto::ClipboardContentType::Files;
    // Path that doesn't exist → size 0 in EncodeFileMetadata. Use a
    // weird-looking path so future global "fix" patterns can't accidentally
    // turn this into a real file.
    snap.file_paths.push_back(L"C:\\__lh_test_does_not_exist__\\a.bin");
    snap.virtual_files.push_back(VirtualFileEntry{L"b.bin", 4096, /*lindex=*/0});

    const auto inner = codec::EncodeClipboardData(snap);
    codec::ParsedClipboardData parsed;
    ASSERT_TRUE(codec::ParseClipboardData(inner.data(), inner.size(), parsed));
    // Both physical + virtual share field 4; we expect two sub-messages.
    EXPECT_EQ(parsed.files.size(), static_cast<std::size_t>(2));
}

TEST(DispatchCodec, EncodeClipboardDataUnspecifiedOmitsPayload) {
    ClipboardSnapshot snap;  // content_type defaults to Unspecified (=0).
    const auto inner = codec::EncodeClipboardData(snap);
    // Should only carry the content_type field (which itself is zero-
    // suppressed by WriteEnumField for Unspecified=0). So expect empty.
    EXPECT_TRUE(inner.empty());
}

// ─── EncodeFileMetadata ───────────────────────────────────────────────────

TEST(DispatchCodec, EncodeFileMetadataExtractsBasenameWithBackslash) {
    const auto bytes = codec::EncodeFileMetadata(
        7, L"C:\\__lh_test_does_not_exist__\\folder\\file.txt");
    pw::Reader r(bytes.data(), bytes.size());
    std::string filename;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == codec::kFMFieldFilename && wire == pw::WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            filename.assign(reinterpret_cast<const char*>(p), n);
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_EQ(filename, std::string("file.txt"));
}

TEST(DispatchCodec, EncodeFileMetadataExtractsBasenameWithForwardSlash) {
    const auto bytes = codec::EncodeFileMetadata(
        0, L"/__lh_test_does_not_exist__/a.bin");
    pw::Reader r(bytes.data(), bytes.size());
    std::string filename;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == codec::kFMFieldFilename && wire == pw::WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            filename.assign(reinterpret_cast<const char*>(p), n);
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_EQ(filename, std::string("a.bin"));
}

TEST(DispatchCodec, EncodeFileMetadataNonExistentPathReportsSizeZero) {
    const auto bytes = codec::EncodeFileMetadata(
        3, L"C:\\__lh_test_does_not_exist__\\nope.bin");
    pw::Reader r(bytes.data(), bytes.size());
    bool saw_size = false;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == codec::kFMFieldFileSize && wire == pw::WireType::Varint) {
            std::uint64_t v = 0;
            ASSERT_TRUE(r.ReadVarint(v));
            // 0 is proto3-suppressed; if we see the field at all it must
            // be a non-zero overlay — that's the bug we'd catch here.
            saw_size = true;
            EXPECT_EQ(v, static_cast<std::uint64_t>(0));
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    // file_size==0 is proto3-suppressed → the field must NOT appear.
    EXPECT_FALSE(saw_size);
}

TEST(DispatchCodec, EncodeFileMetadataIndexUsedAsFileId) {
    const auto bytes = codec::EncodeFileMetadata(42, L"C:\\fake.bin");
    pw::Reader r(bytes.data(), bytes.size());
    std::string file_id;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == codec::kFMFieldFileId && wire == pw::WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            file_id.assign(reinterpret_cast<const char*>(p), n);
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_EQ(file_id, std::string("42"));
}

TEST(DispatchCodec, EncodeVirtualFileMetadataOmitsRelativePath) {
    VirtualFileEntry e{L"virtual.bin", /*size=*/1024, /*lindex=*/0};
    const auto bytes = codec::EncodeVirtualFileMetadata(0, e);
    pw::Reader r(bytes.data(), bytes.size());
    bool saw_relpath = false;
    bool saw_size = false;
    std::uint64_t size_val = 0;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == codec::kFMFieldRelativePath) saw_relpath = true;
        if (field == codec::kFMFieldFileSize && wire == pw::WireType::Varint) {
            ASSERT_TRUE(r.ReadVarint(size_val));
            saw_size = true;
            continue;
        }
        ASSERT_TRUE(r.SkipField(wire));
    }
    EXPECT_FALSE(saw_relpath);
    EXPECT_TRUE(saw_size);
    EXPECT_EQ(size_val, static_cast<std::uint64_t>(1024));
}

// ─── EncodeClipboardEnvelope ──────────────────────────────────────────────

TEST(DispatchCodec, EncodeClipboardEnvelopeChangedRoundtrips) {
    ClipboardSnapshot snap;
    snap.content_type = proto::ClipboardContentType::Text;
    snap.text         = L"x";
    const auto inner = codec::EncodeClipboardData(snap);
    const auto frame = codec::EncodeClipboardEnvelope(
        proto::HelperMessageType::ClipboardChanged, inner, &StubClock);

    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::ClipboardChanged));
    ASSERT_EQ(parsed.clipboard_data_len, inner.size());
    EXPECT_EQ(std::memcmp(parsed.clipboard_data_ptr, inner.data(),
                          inner.size()), 0);
}

TEST(DispatchCodec, EncodeClipboardEnvelopeAllowsEmptyInnerForGetClipboard) {
    // Producing CLIPBOARD_CONTENT with an empty inner payload is the path
    // we take when GET_CLIPBOARD finds an empty clipboard — the envelope
    // must still encode and round-trip cleanly.
    const std::vector<std::uint8_t> empty_inner;
    const auto frame = codec::EncodeClipboardEnvelope(
        proto::HelperMessageType::ClipboardContent, empty_inner, &StubClock);
    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::ClipboardContent));
    EXPECT_EQ(parsed.clipboard_data_len, static_cast<std::size_t>(0));
    EXPECT_TRUE(parsed.clipboard_data_ptr != nullptr);
}

// ─── EncodeDataRequest / EncodeFileChunk* ─────────────────────────────────

TEST(DispatchCodec, EncodeDataRequestEmbedsHashAndType) {
    const auto frame = codec::EncodeDataRequest(
        "deadbeef", proto::ClipboardContentType::Image, &StubClock);
    codec::ParsedHelperMessage outer;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, outer));
    EXPECT_EQ(static_cast<int>(outer.type),
              static_cast<int>(proto::HelperMessageType::DataRequest));
    ASSERT_TRUE(outer.clipboard_data_ptr == nullptr);
    // The DATA_REQUEST sub-message lives on its own field; ParseHelperMessage
    // doesn't currently extract it, so read it directly off the wire.
    pw::Reader r(frame);
    bool saw_request_sub = false;
    while (!r.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(r.ReadTag(field, wire));
        if (field == proto::kFieldDataRequest && wire == pw::WireType::LengthDelim) {
            const std::uint8_t* p = nullptr;
            std::size_t         n = 0;
            ASSERT_TRUE(r.ReadLengthDelim(p, n));
            // Decode the inner sub-message and look for content_hash.
            pw::Reader sub(p, n);
            std::string hash;
            int seen_type = -1;
            while (!sub.eof()) {
                std::uint32_t sf = 0;
                pw::WireType  sw = pw::WireType::Varint;
                ASSERT_TRUE(sub.ReadTag(sf, sw));
                if (sf == proto::kReqFieldContentHash && sw == pw::WireType::LengthDelim) {
                    const std::uint8_t* sp = nullptr;
                    std::size_t         sn = 0;
                    ASSERT_TRUE(sub.ReadLengthDelim(sp, sn));
                    hash.assign(reinterpret_cast<const char*>(sp), sn);
                } else if (sf == proto::kReqFieldContentType && sw == pw::WireType::Varint) {
                    std::uint64_t v = 0;
                    ASSERT_TRUE(sub.ReadVarint(v));
                    seen_type = static_cast<int>(v);
                } else {
                    ASSERT_TRUE(sub.SkipField(sw));
                }
            }
            EXPECT_EQ(hash, std::string("deadbeef"));
            EXPECT_EQ(seen_type, static_cast<int>(proto::ClipboardContentType::Image));
            saw_request_sub = true;
        } else {
            ASSERT_TRUE(r.SkipField(wire));
        }
    }
    EXPECT_TRUE(saw_request_sub);
}

TEST(DispatchCodec, EncodeFileChunkRequestOutboundCarriesFourFields) {
    const auto frame = codec::EncodeFileChunkRequestOutbound(
        "tx-1", "f0", /*offset=*/0x1122334455667788ull, /*size=*/65536u,
        &StubClock);

    codec::ParsedHelperMessage outer;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, outer));
    EXPECT_EQ(static_cast<int>(outer.type),
              static_cast<int>(proto::HelperMessageType::FileChunkRequest));
    ASSERT_TRUE(outer.file_chunk_request_ptr != nullptr);

    codec::ParsedFileChunkRequest req;
    ASSERT_TRUE(codec::ParseFileChunkRequest(
        outer.file_chunk_request_ptr, outer.file_chunk_request_len, req));
    EXPECT_EQ(req.transfer_id, std::string("tx-1"));
    EXPECT_EQ(req.file_id, std::string("f0"));
    EXPECT_EQ(req.offset, static_cast<std::uint64_t>(0x1122334455667788ull));
    EXPECT_EQ(req.size, static_cast<std::uint32_t>(65536u));
}

TEST(DispatchCodec, EncodeFileChunkDataRoundTripWithData) {
    const std::uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    const auto frame = codec::EncodeFileChunkData(
        "tx", "f1", /*offset=*/256, payload, sizeof(payload),
        /*is_last=*/true, /*error=*/{}, &StubClock);

    codec::ParsedHelperMessage outer;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, outer));
    EXPECT_EQ(static_cast<int>(outer.type),
              static_cast<int>(proto::HelperMessageType::FileChunkData));
    ASSERT_TRUE(outer.file_chunk_data_ptr != nullptr);

    codec::ParsedFileChunkData chunk;
    ASSERT_TRUE(codec::ParseFileChunkData(
        outer.file_chunk_data_ptr, outer.file_chunk_data_len, chunk));
    EXPECT_EQ(chunk.transfer_id, std::string("tx"));
    EXPECT_EQ(chunk.file_id, std::string("f1"));
    EXPECT_EQ(chunk.offset, static_cast<std::uint64_t>(256));
    EXPECT_EQ(chunk.data.size(), static_cast<std::size_t>(5));
    EXPECT_EQ(std::memcmp(chunk.data.data(), payload, 5), 0);
    EXPECT_TRUE(chunk.is_last);
    EXPECT_TRUE(chunk.error.empty());
}

TEST(DispatchCodec, EncodeFileChunkDataPropagatesError) {
    const auto frame = codec::EncodeFileChunkData(
        "tx", "f1", /*offset=*/0, /*data=*/nullptr, /*data_len=*/0,
        /*is_last=*/true, /*error=*/"disk read failed", &StubClock);

    codec::ParsedHelperMessage outer;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, outer));
    ASSERT_TRUE(outer.file_chunk_data_ptr != nullptr);

    codec::ParsedFileChunkData chunk;
    ASSERT_TRUE(codec::ParseFileChunkData(
        outer.file_chunk_data_ptr, outer.file_chunk_data_len, chunk));
    EXPECT_TRUE(chunk.data.empty());
    EXPECT_TRUE(chunk.is_last);
    EXPECT_EQ(chunk.error, std::string("disk read failed"));
}

TEST(DispatchCodec, EncodeFileChunkDataIsLastFalseSuppressedOnWire) {
    // is_last=false → proto3 zero-suppression keeps the field off the
    // wire. Walking the inner submessage explicitly catches a regression
    // where the encoder might emit an explicit `0` for is_last; a
    // pure round-trip test would silently pass that.
    const std::uint8_t payload[1] = {0xAA};
    const auto frame = codec::EncodeFileChunkData(
        "tx", "f", /*offset=*/0, payload, 1,
        /*is_last=*/false, /*error=*/{}, &StubClock);

    codec::ParsedHelperMessage outer;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, outer));
    ASSERT_TRUE(outer.file_chunk_data_ptr != nullptr);

    pw::Reader inner(outer.file_chunk_data_ptr, outer.file_chunk_data_len);
    bool saw_is_last_field = false;
    while (!inner.eof()) {
        std::uint32_t field = 0;
        pw::WireType  wire  = pw::WireType::Varint;
        ASSERT_TRUE(inner.ReadTag(field, wire));
        if (field == proto::kFCDataFieldIsLast) saw_is_last_field = true;
        ASSERT_TRUE(inner.SkipField(wire));
    }
    EXPECT_FALSE(saw_is_last_field);

    // Round-trip check is still meaningful: a regression in either the
    // encoder OR the parser would surface as is_last==true here.
    codec::ParsedFileChunkData chunk;
    ASSERT_TRUE(codec::ParseFileChunkData(
        outer.file_chunk_data_ptr, outer.file_chunk_data_len, chunk));
    EXPECT_FALSE(chunk.is_last);
}

// ─── ParseHelperMessage robustness ────────────────────────────────────────

TEST(DispatchCodec, ParseHelperMessageEmptyBufferYieldsUnspecified) {
    const std::vector<std::uint8_t> empty;
    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(empty, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::Unspecified));
    EXPECT_TRUE(parsed.clipboard_data_ptr == nullptr);
}

TEST(DispatchCodec, ParseHelperMessageSkipsUnknownVarintField) {
    // Encode a frame with a Ready type plus an unknown high field (99,
    // varint). ParseHelperMessage must skip the unknown and still read
    // the known type. The forward-compat path is what lets us evolve the
    // schema without breaking older helpers.
    pw::Writer w;
    w.WriteEnumField(proto::kFieldType,
                     static_cast<std::int32_t>(proto::HelperMessageType::Ready));
    w.WriteUint64Field(/*field=*/99, /*value=*/12345);
    const auto frame = w.take();

    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::Ready));
}

TEST(DispatchCodec, ParseHelperMessageRejectsTruncatedFrame) {
    // First byte sets the continuation bit on the tag varint → tag is
    // incomplete, so ParseHelperMessage must fail loudly rather than
    // silently treating the buffer as Unspecified.
    const std::vector<std::uint8_t> bad{0x80};
    codec::ParsedHelperMessage parsed;
    EXPECT_FALSE(codec::ParseHelperMessage(bad, parsed));
}

TEST(DispatchCodec, ParseHelperMessageSkipsTypeWithWrongWireType) {
    // Encode type as a LengthDelim string (wrong wire type for an enum).
    // ParseHelperMessage's branch only matches when wire==Varint, so it
    // must fall into SkipField for the bogus tag. SkipField consumes the
    // length-delim payload and returns true → ParseHelperMessage
    // overall succeeds; out.type remains Unspecified.
    pw::Writer w;
    w.WriteStringField(proto::kFieldType, "bogus");
    const auto frame = w.take();

    codec::ParsedHelperMessage parsed;
    ASSERT_TRUE(codec::ParseHelperMessage(frame, parsed));
    EXPECT_EQ(static_cast<int>(parsed.type),
              static_cast<int>(proto::HelperMessageType::Unspecified));
}

// ─── ParseAnnouncement ────────────────────────────────────────────────────

TEST(DispatchCodec, ParseAnnouncementReadsHashAndTransferId) {
    pw::Writer w;
    w.WriteEnumField(proto::kAnnFieldContentType,
                     static_cast<std::int32_t>(proto::ClipboardContentType::Files));
    w.WriteStringField(proto::kAnnFieldContentHash, "abc123");
    w.WriteStringField(proto::kAnnFieldTransferId, "tx-9");
    const auto bytes = w.take();

    codec::ParsedAnnouncement ann;
    ASSERT_TRUE(codec::ParseAnnouncement(bytes.data(), bytes.size(), ann));
    EXPECT_EQ(static_cast<int>(ann.content_type),
              static_cast<int>(proto::ClipboardContentType::Files));
    EXPECT_EQ(ann.content_hash, std::string("abc123"));
    EXPECT_EQ(ann.transfer_id,  std::string("tx-9"));
    EXPECT_TRUE(ann.files.empty());
}

TEST(DispatchCodec, ParseAnnouncementCollectsRepeatedFileEntries) {
    pw::Writer entry1;
    entry1.WriteStringField(codec::kFMFieldFileId, "0");
    pw::Writer entry2;
    entry2.WriteStringField(codec::kFMFieldFileId, "1");

    pw::Writer ann_w;
    ann_w.WriteEnumField(proto::kAnnFieldContentType,
                         static_cast<std::int32_t>(proto::ClipboardContentType::Files));
    ann_w.WriteSubMessageField(proto::kAnnFieldFiles, entry1.bytes());
    ann_w.WriteSubMessageField(proto::kAnnFieldFiles, entry2.bytes());
    const auto bytes = ann_w.take();

    codec::ParsedAnnouncement ann;
    ASSERT_TRUE(codec::ParseAnnouncement(bytes.data(), bytes.size(), ann));
    EXPECT_EQ(ann.files.size(), static_cast<std::size_t>(2));
}

// ─── ParseFileMetadataForVirtualFile ──────────────────────────────────────

TEST(DispatchCodec, ParseFileMetadataPrefersRelativePathOverFilename) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFileId, "5");
    fm.WriteStringField(codec::kFMFieldFilename, "ignored.txt");
    fm.WriteStringField(codec::kFMFieldRelativePath, "subdir\\preferred.txt");
    fm.WriteUint64Field(codec::kFMFieldFileSize, 99);
    const auto bytes = fm.take();

    VirtualFileSpec spec;
    ASSERT_TRUE(codec::ParseFileMetadataForVirtualFile(
        bytes.data(), bytes.size(), spec));
    EXPECT_EQ(spec.file_id, std::string("5"));
    EXPECT_EQ(spec.size, static_cast<std::uint64_t>(99));
    EXPECT_EQ(spec.name, std::wstring(L"subdir\\preferred.txt"));
    EXPECT_FALSE(spec.is_directory);
}

TEST(DispatchCodec, ParseFileMetadataFallsBackToFilenameWhenRelativePathEmpty) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFileId, "0");
    fm.WriteStringField(codec::kFMFieldFilename, "fallback.txt");
    // relative_path is intentionally absent (or empty — same effect).
    const auto bytes = fm.take();

    VirtualFileSpec spec;
    ASSERT_TRUE(codec::ParseFileMetadataForVirtualFile(
        bytes.data(), bytes.size(), spec));
    EXPECT_EQ(spec.name, std::wstring(L"fallback.txt"));
}

TEST(DispatchCodec, ParseFileMetadataRejectsEmptyFileId) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFilename, "any.txt");
    const auto bytes = fm.take();
    VirtualFileSpec spec;
    EXPECT_FALSE(codec::ParseFileMetadataForVirtualFile(
        bytes.data(), bytes.size(), spec));
}

TEST(DispatchCodec, ParseFileMetadataRejectsBothNameFieldsEmpty) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFileId, "1");
    const auto bytes = fm.take();
    VirtualFileSpec spec;
    EXPECT_FALSE(codec::ParseFileMetadataForVirtualFile(
        bytes.data(), bytes.size(), spec));
}

TEST(DispatchCodec, ParseFileMetadataCarriesIsDirectoryFlag) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFileId, "2");
    fm.WriteStringField(codec::kFMFieldFilename, "stuff");
    fm.WriteBoolField(codec::kFMFieldIsDirectory, true);
    const auto bytes = fm.take();

    VirtualFileSpec spec;
    ASSERT_TRUE(codec::ParseFileMetadataForVirtualFile(
        bytes.data(), bytes.size(), spec));
    EXPECT_TRUE(spec.is_directory);
}

// ─── ParseProvideData ─────────────────────────────────────────────────────

TEST(DispatchCodec, ParseProvideDataRoundTripsHashAndBytes) {
    pw::Writer w;
    w.WriteStringField(proto::kPDFieldContentHash, "hash");
    const std::uint8_t body[3] = {0x01, 0x02, 0x03};
    w.WriteBytesField(proto::kPDFieldData, body, 3);
    const auto bytes = w.take();

    codec::ParsedProvideData pd;
    ASSERT_TRUE(codec::ParseProvideData(bytes.data(), bytes.size(), pd));
    EXPECT_EQ(pd.content_hash, std::string("hash"));
    ASSERT_EQ(pd.data.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(pd.data[0], static_cast<std::uint8_t>(0x01));
    EXPECT_EQ(pd.data[2], static_cast<std::uint8_t>(0x03));
}

TEST(DispatchCodec, ParseProvideDataAllowsEmptyData) {
    pw::Writer w;
    w.WriteStringField(proto::kPDFieldContentHash, "h");
    // Don't emit the data field — empty bytes are proto3-suppressed.
    const auto bytes = w.take();

    codec::ParsedProvideData pd;
    ASSERT_TRUE(codec::ParseProvideData(bytes.data(), bytes.size(), pd));
    EXPECT_TRUE(pd.data.empty());
    EXPECT_EQ(pd.content_hash, std::string("h"));
}

// ─── ParseFileChunkRequest defensive wire-type handling ───────────────────

TEST(DispatchCodec, ParseFileChunkRequestSkipsTransferIdWithWrongWireType) {
    // Build a payload where transfer_id appears with Varint wire type;
    // ParseFileChunkRequest must SkipField the bogus entry and continue.
    pw::Writer w;
    w.WriteUint64Field(proto::kFCReqFieldTransferId, 12345);  // wrong type
    w.WriteStringField(proto::kFCReqFieldFileId, "f");
    const auto bytes = w.take();

    codec::ParsedFileChunkRequest req;
    ASSERT_TRUE(codec::ParseFileChunkRequest(bytes.data(), bytes.size(), req));
    EXPECT_TRUE(req.transfer_id.empty());
    EXPECT_EQ(req.file_id, std::string("f"));
}

// ─── ExtractRelativePath ──────────────────────────────────────────────────

TEST(DispatchCodec, ExtractRelativePathFindsField) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldRelativePath, "C:\\full.txt");
    const auto bytes = fm.take();
    std::string out;
    ASSERT_TRUE(codec::ExtractRelativePath(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out, std::string("C:\\full.txt"));
}

TEST(DispatchCodec, ExtractRelativePathReturnsFalseWhenAbsent) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFilename, "only-name.txt");
    const auto bytes = fm.take();
    std::string out;
    EXPECT_FALSE(codec::ExtractRelativePath(bytes.data(), bytes.size(), out));
    EXPECT_TRUE(out.empty());
}

TEST(DispatchCodec, ExtractRelativePathSkipsUnrelatedFields) {
    pw::Writer fm;
    fm.WriteStringField(codec::kFMFieldFileId, "99");
    fm.WriteUint64Field(codec::kFMFieldFileSize, 1234);
    fm.WriteStringField(codec::kFMFieldRelativePath, "rel");
    const auto bytes = fm.take();
    std::string out;
    ASSERT_TRUE(codec::ExtractRelativePath(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out, std::string("rel"));
}

// ─── Clock injection ──────────────────────────────────────────────────────

TEST(DispatchCodec, NowUnixMillisIsBoundedByCallTime) {
    // Production clock should be in a plausible range — neither zero nor
    // wildly in the future. Lower bound: 2026-01-01 UTC = 1767225600000 ms.
    const std::uint64_t lower = 1767225600000ull;
    const std::uint64_t v = codec::NowUnixMillis();
    EXPECT_GT(v, lower);
}
