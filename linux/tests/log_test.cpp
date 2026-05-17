// log unit tests — focus on the pure FormatLogLine surface. Side-effecting
// LogMessage (writes to stderr) is left to manual smoke verification.

#include "log.h"
#include "test_lite.h"

#include <chrono>
#include <cstring>
#include <string>

using leviathan::clipboard_helper::FormatLogLine;
using leviathan::clipboard_helper::LogLevel;

namespace {

std::chrono::system_clock::time_point FixedTime() {
    // 2026-01-15 12:34:56.789 UTC. The actual rendered hour will depend
    // on the test runner's local timezone — tests only assert the
    // millisecond field and the wrapping structure so the test is TZ-
    // independent.
    using namespace std::chrono;
    auto t = system_clock::from_time_t(1768480496);  // 2026-01-15 13:34:56 UTC (close enough)
    t += milliseconds(789);
    return t;
}

}  // namespace

TEST(Log, FormatLogLineEmitsLevelTag) {
    char buf[256];
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1234,
                                 "hello",
                                 buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    const std::string line(buf, n);
    EXPECT_NE(line.find("[INFO ]"), std::string::npos);
    EXPECT_NE(line.find("[helper:1234]"), std::string::npos);
    EXPECT_NE(line.find("hello"), std::string::npos);
    EXPECT_EQ(line.back(), '\n');
}

TEST(Log, FormatLogLineEmitsAllLevels) {
    struct Case { LogLevel level; const char* tag; };
    const Case cases[] = {
        {LogLevel::Debug, "[DEBUG]"},
        {LogLevel::Info,  "[INFO ]"},
        {LogLevel::Warn,  "[WARN ]"},
        {LogLevel::Error, "[ERROR]"},
    };
    char buf[256];
    for (const auto& c : cases) {
        const auto n = FormatLogLine(c.level, FixedTime(), 1, "x",
                                     buf, sizeof(buf));
        ASSERT_TRUE(n > 0);
        const std::string line(buf, n);
        EXPECT_NE(line.find(c.tag), std::string::npos);
    }
}

TEST(Log, FormatLogLineEmitsMillisecondField) {
    char buf[256];
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1, "x",
                                 buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    const std::string line(buf, n);
    // Layout: "[YYYY-MM-DD HH:MM:SS.789] ...". The millisecond field is
    // independent of the local timezone (TZ shifts only the H/M/S
    // components).
    EXPECT_NE(line.find(".789]"), std::string::npos);
}

TEST(Log, FormatLogLineRejectsZeroSizeBuffer) {
    char buf[1] = {0};
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1, "x",
                                 buf, 0);
    EXPECT_EQ(n, static_cast<std::size_t>(0));
}

TEST(Log, FormatLogLineRejectsNullBuffer) {
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1, "x",
                                 nullptr, 256);
    EXPECT_EQ(n, static_cast<std::size_t>(0));
}

TEST(Log, FormatLogLineClampsToBuffer) {
    // Allocate a buffer too small for the whole line; FormatLogLine
    // must return <= out_size - 1 and NUL-terminate.
    char buf[40];
    const std::string long_msg(1024, 'A');
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1, long_msg,
                                 buf, sizeof(buf));
    EXPECT_LE(n, sizeof(buf) - 1);
    // The buffer must remain NUL-terminated even after the clamp.
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(Log, FormatLogLineHandlesEmptyMessage) {
    char buf[128];
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 1, "",
                                 buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    const std::string line(buf, n);
    // Format still emits the structural prefix.
    EXPECT_NE(line.find("[helper:1]"), std::string::npos);
    EXPECT_EQ(line.back(), '\n');
}

TEST(Log, FormatLogLineHandlesPidZero) {
    char buf[128];
    const auto n = FormatLogLine(LogLevel::Info, FixedTime(), 0, "x",
                                 buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    const std::string line(buf, n);
    EXPECT_NE(line.find("[helper:0]"), std::string::npos);
}
