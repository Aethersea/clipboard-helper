// uri_list_format unit tests — covers the pure helpers shared between
// the wlr / ext / X11 backends for converting newline-joined local
// paths into the bytes a paste consumer expects for text/uri-list
// (RFC 2483) or x-special/gnome-copied-files (Nautilus convention).

#include "test_lite.h"
#include "uri_list_format.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ul = leviathan::clipboard_helper::uri_list;

namespace {

std::vector<std::uint8_t> Bytes(std::string_view s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string AsString(const std::vector<std::uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

}  // namespace

// ─── IsUriPathSafe ────────────────────────────────────────────────────────

TEST(UriList, IsUriPathSafeAcceptsUnreservedPlusSlash) {
    // RFC 3986 unreserved set + '/' as path separator.
    for (char c : std::string("abcXYZ0129-._~/")) {
        EXPECT_TRUE(ul::IsUriPathSafe(static_cast<unsigned char>(c)));
    }
}

TEST(UriList, IsUriPathSafeRejectsReservedAndOther) {
    // Reserved URI delimiters that frequently appear in filenames
    // and MUST be percent-encoded so the URI parses unambiguously.
    for (char c : std::string(" #?&=+%@:;,(){}[]\"'<>\\")) {
        EXPECT_FALSE(ul::IsUriPathSafe(static_cast<unsigned char>(c)));
    }
}

TEST(UriList, IsUriPathSafeRejectsHighBitBytes) {
    // UTF-8 multi-byte continuations and any high-bit byte must be
    // percent-encoded byte-by-byte per RFC 3986 (the spec works at
    // the byte level, not the codepoint level, for "path" segments).
    EXPECT_FALSE(ul::IsUriPathSafe(0x80));
    EXPECT_FALSE(ul::IsUriPathSafe(0xC3));  // first byte of UTF-8 ñ
    EXPECT_FALSE(ul::IsUriPathSafe(0xFF));
}

// ─── PercentEncodePath ────────────────────────────────────────────────────

TEST(UriList, PercentEncodePathLeavesSafeCharsUntouched) {
    EXPECT_EQ(ul::PercentEncodePath("/usr/local/bin/test.txt"),
              std::string("/usr/local/bin/test.txt"));
}

TEST(UriList, PercentEncodePathEncodesSpace) {
    EXPECT_EQ(ul::PercentEncodePath("/tmp/hello world.txt"),
              std::string("/tmp/hello%20world.txt"));
}

TEST(UriList, PercentEncodePathEncodesHashAndQuery) {
    // '#' and '?' would otherwise be parsed as fragment / query
    // delimiters by a URI reader.
    EXPECT_EQ(ul::PercentEncodePath("/x/y#frag.txt"),
              std::string("/x/y%23frag.txt"));
    EXPECT_EQ(ul::PercentEncodePath("/x/y?q.txt"),
              std::string("/x/y%3Fq.txt"));
}

TEST(UriList, PercentEncodePathEncodesUtf8ByteByByte) {
    // "café" → "c" + "a" + "f" + 0xC3 0xA9
    const std::string input = std::string("/tmp/caf") + '\xc3' + '\xa9';
    EXPECT_EQ(ul::PercentEncodePath(input), std::string("/tmp/caf%C3%A9"));
}

TEST(UriList, PercentEncodePathEncodesPercentItself) {
    // Already-percent-encoded paths must be re-encoded so the '%'
    // doesn't appear as an unescaped percent in the URI.
    EXPECT_EQ(ul::PercentEncodePath("/x/50%off.txt"),
              std::string("/x/50%25off.txt"));
}

TEST(UriList, PercentEncodePathHandlesEmptyInput) {
    EXPECT_EQ(ul::PercentEncodePath(""), std::string(""));
}

// ─── SplitPaths ───────────────────────────────────────────────────────────

TEST(UriList, SplitPathsHandlesNewlineSeparated) {
    const auto out = ul::SplitPaths("/a\n/b\n/c");
    ASSERT_EQ(out.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(out[0], std::string("/a"));
    EXPECT_EQ(out[1], std::string("/b"));
    EXPECT_EQ(out[2], std::string("/c"));
}

TEST(UriList, SplitPathsTrimsTrailingCR) {
    // Tolerate CRLF input — '\r\n' or just '\r' at end-of-line is
    // stripped so callers don't have to.
    const auto out = ul::SplitPaths("/a\r\n/b\r\n/c");
    ASSERT_EQ(out.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(out[0], std::string("/a"));
    EXPECT_EQ(out[1], std::string("/b"));
    EXPECT_EQ(out[2], std::string("/c"));
}

TEST(UriList, SplitPathsSkipsEmptyLines) {
    // Trailing newline + double-newline produce empty segments — we
    // skip them so the output doesn't contain phantom blank paths.
    const auto out = ul::SplitPaths("/a\n\n/b\n");
    ASSERT_EQ(out.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(out[0], std::string("/a"));
    EXPECT_EQ(out[1], std::string("/b"));
}

TEST(UriList, SplitPathsHandlesEmptyInput) {
    EXPECT_TRUE(ul::SplitPaths("").empty());
}

TEST(UriList, SplitPathsSinglePathNoSeparator) {
    const auto out = ul::SplitPaths("/only/one");
    ASSERT_EQ(out.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(out[0], std::string("/only/one"));
}

// ─── MakeFileUri ──────────────────────────────────────────────────────────

TEST(UriList, MakeFileUriEmitsPrefixAndPercentEncoding) {
    EXPECT_EQ(ul::MakeFileUri("/tmp/hello world.txt"),
              std::string("file:///tmp/hello%20world.txt"));
}

// ─── FormatTextUriList (RFC 2483) ────────────────────────────────────────

TEST(UriList, FormatTextUriListCrlfTerminatedIncludingLast) {
    const std::vector<std::string> paths = {"/a", "/b", "/c"};
    EXPECT_EQ(ul::FormatTextUriList(paths),
              std::string("file:///a\r\nfile:///b\r\nfile:///c\r\n"));
}

TEST(UriList, FormatTextUriListEmptyInput) {
    EXPECT_EQ(ul::FormatTextUriList({}), std::string(""));
}

TEST(UriList, FormatTextUriListSinglePath) {
    EXPECT_EQ(ul::FormatTextUriList({"/single"}),
              std::string("file:///single\r\n"));
}

// ─── FormatGnomeCopiedFiles ──────────────────────────────────────────────

TEST(UriList, FormatGnomeCopiedFilesStartsWithCopy) {
    // Nautilus parses the first line as the operation; we always
    // produce "copy" (shen has no cut semantics).
    const auto out = ul::FormatGnomeCopiedFiles({"/a"});
    EXPECT_EQ(out, std::string("copy\nfile:///a"));
}

TEST(UriList, FormatGnomeCopiedFilesNoTrailingNewline) {
    // Trailing LF would produce a phantom empty URI entry in
    // Nautilus's parser; verify we omit it.
    const auto out = ul::FormatGnomeCopiedFiles({"/a", "/b"});
    EXPECT_EQ(out, std::string("copy\nfile:///a\nfile:///b"));
    EXPECT_NE(out.back(), '\n');
}

TEST(UriList, FormatGnomeCopiedFilesEmptyPaths) {
    // No paths still produces a valid header. (Edge case; in practice
    // the caller wouldn't invoke us with an empty list.)
    EXPECT_EQ(ul::FormatGnomeCopiedFiles({}), std::string("copy"));
}

// ─── FormatForMime (umbrella) ────────────────────────────────────────────

TEST(UriList, FormatForMimeTextUriListEndToEnd) {
    const auto out = ul::FormatForMime(
        Bytes("/tmp/a\n/tmp/b with space.txt\n"),
        "text/uri-list");
    EXPECT_EQ(AsString(out),
              std::string("file:///tmp/a\r\nfile:///tmp/b%20with%20space.txt\r\n"));
}

TEST(UriList, FormatForMimeGnomeCopiedFilesEndToEnd) {
    const auto out = ul::FormatForMime(
        Bytes("/tmp/a\n/tmp/b\n"),
        "x-special/gnome-copied-files");
    EXPECT_EQ(AsString(out), std::string("copy\nfile:///tmp/a\nfile:///tmp/b"));
}

TEST(UriList, FormatForMimeUnknownMimeFallsBackToUriList) {
    // Defensive: backends should only ever pass advertised MIMEs,
    // but an unknown one should produce the safer text/uri-list
    // representation rather than empty output.
    const auto out = ul::FormatForMime(
        Bytes("/x/y"), "image/png");
    EXPECT_EQ(AsString(out), std::string("file:///x/y\r\n"));
}

TEST(UriList, FormatForMimeEmptyPathBytesProducesEmpty) {
    const auto out = ul::FormatForMime({}, "text/uri-list");
    EXPECT_TRUE(out.empty());
}

TEST(UriList, FormatForMimeStripsCRLFOnInput) {
    // Parent might one day send CRLF-separated paths instead of LF;
    // SplitPaths tolerates both. Verify end-to-end with the umbrella
    // formatter.
    const auto out = ul::FormatForMime(
        Bytes("/a\r\n/b\r\n"),
        "text/uri-list");
    EXPECT_EQ(AsString(out), std::string("file:///a\r\nfile:///b\r\n"));
}
