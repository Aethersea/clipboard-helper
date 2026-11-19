// uri_list inbound parsing tests (issue #13): DecodeLocalFileUri and
// ParseLocalFileUriList turn a copied text/uri-list into absolute local
// paths, or nullopt when the clipboard should keep being treated as TEXT.

#include "uri_list_format.h"

#include <optional>
#include <string>
#include <vector>

#include "test_lite.h"

namespace ul = leviathan::clipboard_helper::uri_list;

namespace {

using Paths = std::vector<std::string>;

std::optional<Paths> Parse(const std::string& s) { return ul::ParseLocalFileUriList(s); }

}  // namespace

// ─── DecodeLocalFileUri ──────────────────────────────────────────────────

TEST(UriListParse, DecodeEmptyAuthority) {
    const auto p = ul::DecodeLocalFileUri("file:///home/u/a.txt");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/home/u/a.txt"));
}

TEST(UriListParse, DecodeLocalhostAuthority) {
    const auto p = ul::DecodeLocalFileUri("file://localhost/tmp/x");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/x"));
}

TEST(UriListParse, DecodeAuthorityLessForm) {
    const auto p = ul::DecodeLocalFileUri("file:/tmp/x");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/x"));
}

TEST(UriListParse, DecodeSchemeAndHostAreCaseInsensitive) {
    const auto a = ul::DecodeLocalFileUri("FILE:///tmp/Case");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, std::string("/tmp/Case"));  // path case preserved
    const auto b = ul::DecodeLocalFileUri("File://LocalHost/tmp/y");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, std::string("/tmp/y"));
}

TEST(UriListParse, DecodePercentEscapes) {
    const auto p = ul::DecodeLocalFileUri("file:///tmp/a%20b%2Fc%25");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/a b/c%"));
    // Lower and upper hex digits both accepted.
    const auto q = ul::DecodeLocalFileUri("file:///tmp/%e4%B8%AD");
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(*q, std::string("/tmp/\xE4\xB8\xAD"));
}

TEST(UriListParse, DecodeUtf8EscapesBecomeRawBytes) {
    // U+4E2D U+6587 as percent-encoded UTF-8.
    const auto p = ul::DecodeLocalFileUri("file:///tmp/%E4%B8%AD%E6%96%87.txt");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/\xE4\xB8\xAD\xE6\x96\x87.txt"));
}

TEST(UriListParse, DecodeNonUtf8BytesSurvive) {
    const auto p = ul::DecodeLocalFileUri("file:///tmp/%FF%FE");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/\xFF\xFE"));
}

TEST(UriListParse, DecodeStripsTrailingSlashOfDirectory) {
    const auto p = ul::DecodeLocalFileUri("file:///tmp/dir/");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/dir"));
}

TEST(UriListParse, DecodeKeepsRootSlash) {
    const auto p = ul::DecodeLocalFileUri("file:///");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/"));
}

TEST(UriListParse, DecodeRejectsOtherSchemes) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("https://example.com/a").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("http://localhost/a").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("ftp://h/a").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("/tmp/plain/path").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("").has_value());
}

TEST(UriListParse, DecodeRejectsRemoteHost) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("file://otherhost/tmp/x").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("file://192.168.1.2/tmp/x").has_value());
}

TEST(UriListParse, DecodeRejectsRelativePath) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:relative/path").has_value());
}

TEST(UriListParse, DecodeRejectsMalformedEscapes) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/%").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/%2").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/%zz").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/%G1").has_value());
}

TEST(UriListParse, DecodeRejectsNulEscape) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/a%00b").has_value());
}

TEST(UriListParse, DecodeRejectsRawQueryOrFragment) {
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/a?b").has_value());
    EXPECT_FALSE(ul::DecodeLocalFileUri("file:///tmp/a#b").has_value());
}

TEST(UriListParse, DecodeAcceptsEscapedQueryAndFragmentChars) {
    const auto p = ul::DecodeLocalFileUri("file:///tmp/a%3Fb%23c");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, std::string("/tmp/a?b#c"));
}

// ─── ParseLocalFileUriList ───────────────────────────────────────────────

TEST(UriListParse, ListCrlfSeparated) {
    const auto r = Parse("file:///tmp/a\r\nfile:///tmp/b\r\n");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), static_cast<std::size_t>(2));
    EXPECT_EQ((*r)[0], std::string("/tmp/a"));
    EXPECT_EQ((*r)[1], std::string("/tmp/b"));
}

TEST(UriListParse, ListLfSeparatedNoTrailingTerminator) {
    const auto r = Parse("file:///tmp/a\nfile:///tmp/b");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), static_cast<std::size_t>(2));
    EXPECT_EQ((*r)[1], std::string("/tmp/b"));
}

TEST(UriListParse, ListSkipsBlankAndCommentLines) {
    const auto r = Parse("# comment\r\n\r\nfile:///tmp/a\r\n   \r\n#another\r\nfile:///tmp/b\r\n\r\n");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), static_cast<std::size_t>(2));
    EXPECT_EQ((*r)[0], std::string("/tmp/a"));
    EXPECT_EQ((*r)[1], std::string("/tmp/b"));
}

TEST(UriListParse, ListPreservesOrder) {
    const auto r = Parse("file:///z\nfile:///a\nfile:///m\n");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), static_cast<std::size_t>(3));
    EXPECT_EQ((*r)[0], std::string("/z"));
    EXPECT_EQ((*r)[1], std::string("/a"));
    EXPECT_EQ((*r)[2], std::string("/m"));
}

TEST(UriListParse, ListToleratesTrailingNulAndWhitespace) {
    std::string s = "  file:///tmp/a \t\r\nfile:///tmp/b\r\n";
    s.push_back('\0');
    s.push_back('\0');
    const auto r = Parse(s);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), static_cast<std::size_t>(2));
    EXPECT_EQ((*r)[0], std::string("/tmp/a"));
    EXPECT_EQ((*r)[1], std::string("/tmp/b"));
}

TEST(UriListParse, ListDirectoryTrailingSlashDropped) {
    const auto r = Parse("file:///tmp/dir/\r\nfile:///tmp/f\r\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)[0], std::string("/tmp/dir"));
}

TEST(UriListParse, ListDecodesUtf8) {
    const auto r = Parse("file:///tmp/%E4%B8%AD%20x\r\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)[0], std::string("/tmp/\xE4\xB8\xAD x"));
}

TEST(UriListParse, ListRejectsWhenAnyLineIsNotFileUri) {
    EXPECT_FALSE(Parse("https://example.com/a\r\n").has_value());
    // Mixed: one local file + one web link -> whole list is not files.
    EXPECT_FALSE(Parse("file:///tmp/a\r\nhttps://example.com/b\r\n").has_value());
    EXPECT_FALSE(Parse("https://example.com/b\r\nfile:///tmp/a\r\n").has_value());
}

TEST(UriListParse, ListRejectsRemoteHost) {
    EXPECT_FALSE(Parse("file:///tmp/a\r\nfile://otherhost/tmp/b\r\n").has_value());
}

TEST(UriListParse, ListRejectsMalformedEscapeAndNul) {
    EXPECT_FALSE(Parse("file:///tmp/a\r\nfile:///tmp/%zz\r\n").has_value());
    EXPECT_FALSE(Parse("file:///tmp/%00\r\n").has_value());
}

TEST(UriListParse, ListRejectsRawQueryOrFragment) {
    EXPECT_FALSE(Parse("file:///tmp/a?x=1\r\n").has_value());
    EXPECT_FALSE(Parse("file:///tmp/a#frag\r\n").has_value());
}

TEST(UriListParse, ListRejectsZeroUris) {
    EXPECT_FALSE(Parse("").has_value());
    EXPECT_FALSE(Parse("\r\n\r\n").has_value());
    EXPECT_FALSE(Parse("# only a comment\r\n").has_value());
    EXPECT_FALSE(Parse("   \n \t \n").has_value());
}

TEST(UriListParse, ListRoundTripsWithFormatter) {
    // Outbound formatter and inbound parser agree, including awkward names.
    const Paths in{"/tmp/plain.txt", "/tmp/with space/\xE4\xB8\xAD.txt",
                   "/tmp/q?x#y%z", "/tmp/\xFF\xFE.bin"};
    const auto text = ul::FormatTextUriList(in);
    const auto out  = Parse(text);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->size(), in.size());
    for (std::size_t i = 0; i < in.size() && i < out->size(); ++i) {
        EXPECT_EQ((*out)[i], in[i]);
    }
}
