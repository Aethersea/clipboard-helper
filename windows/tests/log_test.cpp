// FormatLogLine — exercises the pure formatter that LogMessage delegates to.
// The I/O side (WriteFile + OutputDebugStringA) stays untested; only the
// textual line construction is checked here.

#include "log.h"
#include "test_lite.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

using leviathan::clipboard_helper::FormatLogLine;
using leviathan::clipboard_helper::LogLevel;

namespace {

// Build a deterministic system_clock::time_point from a calendar date.
// Used so every test pins the timestamp portion of the formatted line.
std::chrono::system_clock::time_point MakeLocalTime(int y, int mo, int d,
                                                    int h, int mi, int s,
                                                    int ms) {
    std::tm tm_local{};
    tm_local.tm_year  = y - 1900;
    tm_local.tm_mon   = mo - 1;
    tm_local.tm_mday  = d;
    tm_local.tm_hour  = h;
    tm_local.tm_min   = mi;
    tm_local.tm_sec   = s;
    tm_local.tm_isdst = -1;  // let the CRT figure out DST for the runner's TZ
    const std::time_t t = std::mktime(&tm_local);
    return std::chrono::system_clock::from_time_t(t)
         + std::chrono::milliseconds(ms);
}

bool Contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

}  // namespace

// ─── Level tag mapping ────────────────────────────────────────────────────

TEST(Log, EmitsInfoTag) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 12, 34, 56, 789);
    const auto n    = FormatLogLine(LogLevel::Info, when, 4242,
                                    std::string_view("hello"),
                                    buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "[INFO ]"));
}

TEST(Log, EmitsWarnTag) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Warn, when, 1, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "[WARN ]"));
}

TEST(Log, EmitsErrorTag) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Error, when, 1, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "[ERROR]"));
}

TEST(Log, EmitsDebugTag) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Debug, when, 1, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "[DEBUG]"));
}

TEST(Log, LevelTagsAreFixedWidth) {
    // Multiple LogMessage emissions land back-to-back in the same stream,
    // so the tag block must be a uniform width regardless of level. The
    // production tags are padded with trailing spaces for INFO/WARN.
    const struct { LogLevel level; std::string_view tag; } cases[] = {
        {LogLevel::Debug, "[DEBUG]"},
        {LogLevel::Info,  "[INFO ]"},
        {LogLevel::Warn,  "[WARN ]"},
        {LogLevel::Error, "[ERROR]"},
    };
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    for (const auto& c : cases) {
        char buf[256];
        const auto n = FormatLogLine(c.level, when, 1, "x", buf, sizeof(buf));
        ASSERT_TRUE(n > 0);
        EXPECT_EQ(c.tag.size(), static_cast<std::size_t>(7));
        EXPECT_TRUE(Contains(std::string_view(buf, n), c.tag));
    }
}

// ─── Timestamp ────────────────────────────────────────────────────────────

TEST(Log, TimestampMatchesProvidedClock) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 12, 34, 56, 789);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "2026-05-16 12:34:56.789"));
}

TEST(Log, MillisecondsZeroPadded) {
    // 7 ms must render as "007", not "7", so log greps stay column-aligned.
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 12, 34, 56, 7);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), ".007]"));
}

// ─── PID rendering ────────────────────────────────────────────────────────

TEST(Log, PidEmbeddedAsDecimal) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 12345, "x", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(Contains(std::string_view(buf, n), "[helper:12345]"));
}

// ─── Message body ─────────────────────────────────────────────────────────

TEST(Log, BodyEndsWithLineFeed) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, "hello", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_EQ(buf[n - 1], '\n');
}

TEST(Log, EmptyMessageEmitsHeaderOnly) {
    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, std::string_view(),
                                    buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    // Header + " " + "" + "\n"; check the body is exactly the LF.
    const std::string_view line(buf, n);
    EXPECT_EQ(line.back(), '\n');
    // The space-LF pair means there is no message text between them.
    EXPECT_TRUE(line.size() >= 2);
    EXPECT_EQ(line[line.size() - 2], ' ');
}

TEST(Log, MessageWithEmbeddedNulIsTruncatedAtFirstNul) {
    // The formatter is implemented with snprintf("%.*s", ...) which the C
    // standard defines to stop at the first NUL inside the source even
    // when an explicit precision was provided. The behaviour is locked in
    // here so any future switch to a memcpy-based body (which would
    // preserve embedded NULs) is forced to update this expectation
    // alongside the impl.
    const char raw[] = {'a', '\0', 'b'};
    const std::string_view msg(raw, sizeof(raw));

    char buf[256];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, msg, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);

    const std::string_view header_end_marker = "[helper:1] ";
    const std::string_view line(buf, n);
    const auto pos = line.find(header_end_marker);
    ASSERT_TRUE(pos != std::string_view::npos);
    const std::size_t body_start = pos + header_end_marker.size();

    // Body should be just "a\n" — the '\0' terminates the conversion,
    // dropping 'b'.
    ASSERT_EQ(line.size() - body_start, static_cast<std::size_t>(2));
    EXPECT_EQ(line[body_start + 0], 'a');
    EXPECT_EQ(line[body_start + 1], '\n');
}

// ─── Truncation guard ─────────────────────────────────────────────────────

TEST(Log, OversizedMessageReportsClampedLength) {
    // Production buffer is 2048 bytes. A 3000-byte message must not cause
    // FormatLogLine to claim more bytes than fit; otherwise WriteFile reads
    // past the buffer. Mirror the production sizing here.
    char buf[2048];
    const std::string huge(3000, 'X');
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, huge, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_LE(n, static_cast<std::size_t>(sizeof(buf) - 1));
}

TEST(Log, OutputAlwaysNulTerminated) {
    char buf[256];
    const std::string body(1000, 'Y');  // forces snprintf to truncate
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, body, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

// ─── Defensive: invalid output buffer ─────────────────────────────────────

TEST(Log, ReturnsZeroOnNullBuffer) {
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, "x", nullptr, 256);
    EXPECT_EQ(n, static_cast<std::size_t>(0));
}

TEST(Log, ReturnsZeroOnZeroSizedBuffer) {
    char buf[1];
    const auto when = MakeLocalTime(2026, 5, 16, 0, 0, 0, 0);
    const auto n    = FormatLogLine(LogLevel::Info, when, 1, "x", buf, 0);
    EXPECT_EQ(n, static_cast<std::size_t>(0));
}
