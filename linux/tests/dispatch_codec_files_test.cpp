// dispatch_codec tests for the FILES / FILE_CHUNK additions (issue #13).
// Wire bytes are decoded with the independent decoder in wire_decode.h
// (field numbers taken straight from Proto/*.proto).

#include "dispatch_codec.h"

#include <cstdint>
#include <string>
#include <vector>

#include "proto_wire.h"
#include "test_lite.h"
#include "wire_decode.h"

namespace cc = leviathan::clipboard_helper::dispatch_codec;
namespace pw = leviathan::clipboard_helper::proto_wire;
namespace wt = wiretest;

namespace {

std::uint64_t StubClock() { return 0x1122334455ull; }

std::vector<std::uint8_t> Bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

// ─── EncodeClipboardChangedFiles ─────────────────────────────────────────

TEST(CodecFiles, ChangedFilesEnvelopeAndFields) {
    std::vector<cc::LocalFileMetadata> files(2);
    files[0].file_id       = "0";
    files[0].filename      = "a.txt";
    files[0].relative_path = "/tmp/x/a.txt";
    files[0].file_size     = 123456789012ull;  // needs a multi-byte varint
    files[0].is_directory  = false;
    files[1].file_id       = "1";
    files[1].filename      = "d";
    files[1].relative_path = "/tmp/x/d";
    files[1].file_size     = 4096;
    files[1].is_directory  = true;

    const auto bytes = cc::EncodeClipboardChangedFiles(files, "deadbeef", StubClock);

    // Raw prefix: field 1 varint (tag 0x08) with value 10.
    ASSERT_TRUE(bytes.size() > 2);
    EXPECT_EQ(static_cast<int>(bytes[0]), 0x08);
    EXPECT_EQ(static_cast<int>(bytes[1]), 0x0A);

    const auto f = wt::Decode(bytes);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChanged);
    EXPECT_EQ(f.timestamp, static_cast<std::uint64_t>(0x1122334455ull));
    ASSERT_TRUE(f.has_clipboard_data);
    EXPECT_EQ(f.content_type, wt::kFiles);
    EXPECT_EQ(f.payload, std::string());
    EXPECT_EQ(f.content_hash, std::string("deadbeef"));
    ASSERT_EQ(f.files.size(), static_cast<std::size_t>(2));

    EXPECT_EQ(f.files[0].file_id, std::string("0"));
    EXPECT_EQ(f.files[0].filename, std::string("a.txt"));
    EXPECT_EQ(f.files[0].relative_path, std::string("/tmp/x/a.txt"));
    EXPECT_EQ(f.files[0].file_size, static_cast<std::uint64_t>(123456789012ull));
    EXPECT_FALSE(f.files[0].is_directory);
    EXPECT_EQ(f.files[0].mime_type, std::string());
    EXPECT_EQ(f.files[0].checksum, std::string());

    EXPECT_EQ(f.files[1].file_id, std::string("1"));
    EXPECT_EQ(f.files[1].filename, std::string("d"));
    EXPECT_EQ(f.files[1].relative_path, std::string("/tmp/x/d"));
    EXPECT_EQ(f.files[1].file_size, static_cast<std::uint64_t>(4096));
    EXPECT_TRUE(f.files[1].is_directory);
}

TEST(CodecFiles, ChangedFilesParsesBackThroughHelperParser) {
    std::vector<cc::LocalFileMetadata> files(1);
    files[0].file_id = "0";
    files[0].filename = "a";
    files[0].relative_path = "/a";
    const auto bytes = cc::EncodeClipboardChangedFiles(files, "h", StubClock);

    cc::ParsedHelperMessage m{};
    ASSERT_TRUE(cc::ParseHelperMessage(bytes, m));
    EXPECT_EQ(static_cast<int>(m.type), 10);
    ASSERT_TRUE(m.clipboard_data_ptr != nullptr);
    cc::ParsedClipboardData cd{};
    ASSERT_TRUE(cc::ParseClipboardData(m.clipboard_data_ptr, m.clipboard_data_len, cd));
    EXPECT_EQ(static_cast<int>(cd.content_type), 3);
    EXPECT_EQ(cd.content_hash, std::string("h"));
    EXPECT_EQ(cd.payload_len, static_cast<std::size_t>(0));
}

TEST(CodecFiles, ChangedFilesBinarySafeNames) {
    std::vector<cc::LocalFileMetadata> files(1);
    files[0].file_id       = "0";
    files[0].filename      = "\xE4\xB8\xAD\xE6\x96\x87 \xFF.bin";
    files[0].relative_path = std::string("/t/") + files[0].filename;
    const auto f = wt::Decode(cc::EncodeClipboardChangedFiles(files, "h", StubClock));
    ASSERT_TRUE(f.ok);
    ASSERT_EQ(f.files.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(f.files[0].filename, files[0].filename);
    EXPECT_EQ(f.files[0].relative_path, files[0].relative_path);
}

// ─── EncodeFileChunkData ─────────────────────────────────────────────────

TEST(CodecFiles, ChunkDataFields) {
    const auto data = Bytes("hello chunk");
    const auto bytes = cc::EncodeFileChunkData("tx-1", "7", 1ull << 33, data.data(),
                                               data.size(), /*is_last=*/true, "", StubClock);
    ASSERT_TRUE(bytes.size() > 2);
    EXPECT_EQ(static_cast<int>(bytes[0]), 0x08);
    EXPECT_EQ(static_cast<int>(bytes[1]), 0x10);  // type 16

    const auto f = wt::Decode(bytes);
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChunkData);
    EXPECT_EQ(f.timestamp, static_cast<std::uint64_t>(0x1122334455ull));
    ASSERT_TRUE(f.has_chunk);
    EXPECT_EQ(f.transfer_id, std::string("tx-1"));
    EXPECT_EQ(f.file_id, std::string("7"));
    EXPECT_EQ(f.offset, static_cast<std::uint64_t>(1ull << 33));
    EXPECT_EQ(f.data, std::string("hello chunk"));
    EXPECT_TRUE(f.is_last);
    EXPECT_EQ(f.error, std::string());
}

TEST(CodecFiles, ChunkDataNotLastKeepsBinaryBytes) {
    std::string raw;
    for (int i = 0; i < 256; ++i) raw.push_back(static_cast<char>(i));
    const auto data = Bytes(raw);
    const auto f = wt::Decode(cc::EncodeFileChunkData("t", "0", 5, data.data(), data.size(),
                                                      false, "", StubClock));
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.data, raw);
    EXPECT_FALSE(f.is_last);
    EXPECT_EQ(f.offset, static_cast<std::uint64_t>(5));
}

TEST(CodecFiles, ChunkDataErrorForm) {
    const auto f = wt::Decode(cc::EncodeFileChunkData("t", "9", 3, nullptr, 0, true,
                                                      "unknown file_id", StubClock));
    ASSERT_TRUE(f.ok);
    EXPECT_EQ(f.type, wt::kChunkData);
    EXPECT_EQ(f.error, std::string("unknown file_id"));
    EXPECT_EQ(f.data, std::string());
    EXPECT_TRUE(f.is_last);
    EXPECT_EQ(f.file_id, std::string("9"));
    EXPECT_EQ(f.offset, static_cast<std::uint64_t>(3));
}

TEST(CodecFiles, ChunkDataParsesAsEnvelopeWithChunkPayload) {
    const auto bytes = cc::EncodeFileChunkData("t", "0", 0, nullptr, 0, true, "", StubClock);
    cc::ParsedHelperMessage m{};
    ASSERT_TRUE(cc::ParseHelperMessage(bytes, m));
    EXPECT_EQ(static_cast<int>(m.type), 16);
    EXPECT_TRUE(m.file_chunk_data_ptr != nullptr);
}

// ─── ParseFileChunkRequest ───────────────────────────────────────────────

namespace {

std::vector<std::uint8_t> ReqBody(const std::string& tid, const std::string& fid,
                                  std::uint64_t off, std::uint32_t size) {
    pw::Writer w;
    w.WriteStringField(1, tid);
    w.WriteStringField(2, fid);
    w.WriteUint64Field(3, off);
    w.WriteUint32Field(4, size);
    return w.take();
}

}  // namespace

TEST(CodecFiles, ParseChunkRequestAllFields) {
    const auto body = ReqBody("tr-9", "12", 5000000000ull, 65536);
    cc::ParsedFileChunkRequest r{};
    ASSERT_TRUE(cc::ParseFileChunkRequest(body.data(), body.size(), r));
    EXPECT_EQ(r.transfer_id, std::string("tr-9"));
    EXPECT_EQ(r.file_id, std::string("12"));
    EXPECT_EQ(r.offset, static_cast<std::uint64_t>(5000000000ull));
    EXPECT_EQ(r.size, static_cast<std::uint32_t>(65536));
}

TEST(CodecFiles, ParseChunkRequestOmittedFieldsDefault) {
    pw::Writer w;
    w.WriteStringField(2, "3");
    const auto body = w.take();
    cc::ParsedFileChunkRequest r{};
    r.offset = 99;
    r.size   = 99;
    ASSERT_TRUE(cc::ParseFileChunkRequest(body.data(), body.size(), r));
    EXPECT_EQ(r.file_id, std::string("3"));
    EXPECT_EQ(r.transfer_id, std::string());
    EXPECT_EQ(r.offset, static_cast<std::uint64_t>(0));
    EXPECT_EQ(r.size, static_cast<std::uint32_t>(0));
}

TEST(CodecFiles, ParseChunkRequestViaEnvelope) {
    const auto frame = wt::ChunkRequest("tx", "4", 10, 20);
    cc::ParsedHelperMessage m{};
    ASSERT_TRUE(cc::ParseHelperMessage(frame, m));
    EXPECT_EQ(static_cast<int>(m.type), 15);
    ASSERT_TRUE(m.file_chunk_request_ptr != nullptr);
    cc::ParsedFileChunkRequest r{};
    ASSERT_TRUE(cc::ParseFileChunkRequest(m.file_chunk_request_ptr, m.file_chunk_request_len, r));
    EXPECT_EQ(r.transfer_id, std::string("tx"));
    EXPECT_EQ(r.file_id, std::string("4"));
    EXPECT_EQ(r.offset, static_cast<std::uint64_t>(10));
    EXPECT_EQ(r.size, static_cast<std::uint32_t>(20));
}

TEST(CodecFiles, ParseChunkRequestRejectsTruncatedLengthDelim) {
    auto body = ReqBody("tr", "id", 1, 2);
    // Cut in the middle of the first string: tag + len(2) + one byte.
    body.resize(3);
    cc::ParsedFileChunkRequest r{};
    EXPECT_FALSE(cc::ParseFileChunkRequest(body.data(), body.size(), r));
}

TEST(CodecFiles, ParseChunkRequestRejectsLengthPastEnd) {
    const std::uint8_t body[] = {0x0A, 0x7F, 'a', 'b'};  // field 1, len 127, 2 bytes
    cc::ParsedFileChunkRequest r{};
    EXPECT_FALSE(cc::ParseFileChunkRequest(body, sizeof(body), r));
}

TEST(CodecFiles, ParseChunkRequestRejectsTruncatedVarint) {
    const std::uint8_t body[] = {0x18, 0x80};  // field 3 varint, continuation bit then EOF
    cc::ParsedFileChunkRequest r{};
    EXPECT_FALSE(cc::ParseFileChunkRequest(body, sizeof(body), r));
}

TEST(CodecFiles, ParseChunkRequestSkipsUnknownFields) {
    // proto3 forward compatibility: unknown fields are ignored, known ones
    // around them still decode.
    pw::Writer w;
    w.WriteStringField(2, "5");
    w.WriteStringField(99, "future");
    w.WriteUint64Field(3, 77);
    const auto body = w.take();
    cc::ParsedFileChunkRequest r{};
    ASSERT_TRUE(cc::ParseFileChunkRequest(body.data(), body.size(), r));
    EXPECT_EQ(r.file_id, std::string("5"));
    EXPECT_EQ(r.offset, static_cast<std::uint64_t>(77));
}
