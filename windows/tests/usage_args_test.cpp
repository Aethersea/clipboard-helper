// Contract tests for the early usage gate, independent of later value parsing.
#include <gtest/gtest.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "usage_args.h"

using leviathan::clipboard_helper::HandleUsageArgs;

namespace {

// Redirect the CRT descriptor underlying FILE*, so fputws/fwprintf are captured
// too. Restore it on every path, including exceptions, before GTest reports.
// These process-wide captures are used only on the test runner's main thread.
class FileCapture {
public:
    explicit FileCapture(FILE* stream) : stream_(stream) {
        if (std::fflush(stream_) != 0 || tmpfile_s(&file_) != 0) {
            throw std::runtime_error("Cannot create stream capture");
        }
        saved_ = _dup(_fileno(stream_));
        if (saved_ == -1 || _dup2(_fileno(file_), _fileno(stream_)) != 0) {
            if (saved_ != -1) _close(saved_);
            std::fclose(file_);
            throw std::runtime_error("Cannot redirect stream");
        }
        // tmpfile_s opens in binary mode. Use the normal console text mode
        // for wide stdio conversion; binary mode would emit UTF-16 bytes.
        _setmode(_fileno(stream_), _O_TEXT);
    }

    ~FileCapture() {
        std::fflush(stream_);
        _dup2(saved_, _fileno(stream_));
        _close(saved_);
        std::fclose(file_);
    }

    FileCapture(const FileCapture&) = delete;
    FileCapture& operator=(const FileCapture&) = delete;

    std::string Read() {
        if (std::fflush(stream_) != 0 || std::fseek(file_, 0, SEEK_SET) != 0) {
            throw std::runtime_error("Cannot rewind stream capture");
        }
        std::string output;
        char buffer[1024];
        while (const auto size = std::fread(buffer, 1, sizeof(buffer), file_)) {
            output.append(buffer, size);
        }
        if (std::ferror(file_)) throw std::runtime_error("Cannot read stream capture");
        return output;
    }

private:
    FILE* stream_;
    FILE* file_ = nullptr;
    int saved_ = -1;
};

struct Result {
    int exit_code;
    std::string out;
    std::string err;
};

Result RunUsage(std::vector<std::wstring> args) {
    std::vector<wchar_t*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    const int argc = static_cast<int>(argv.size());
    argv.push_back(nullptr);
    FileCapture out(stdout);
    FileCapture err(stderr);
    const int code = HandleUsageArgs(argc, argv.data());
    return {code, out.Read(), err.Read()};
}

void ExpectBanner(const std::string& output) {
    // Meaningful banner contents, without coupling to wrapping or whitespace.
    for (const char* text : {"Usage:", "leviathan-clipboard-helper", "--pipe-name=",
                             "--parent-pid=", "--parent-handle=", "--help", "-h"}) {
        EXPECT_NE(output.find(text), std::string::npos) << text;
    }
}

void ExpectContinue(const Result& result) {
    EXPECT_EQ(result.exit_code, -1);
    EXPECT_TRUE(result.out.empty()) << result.out;
    EXPECT_TRUE(result.err.empty()) << result.err;
}

void ExpectHelp(const Result& result) {
    EXPECT_EQ(result.exit_code, 0);
    ExpectBanner(result.out);
    EXPECT_EQ(result.out.find("unknown argument:"), std::string::npos);
    EXPECT_TRUE(result.err.empty()) << result.err;
}

void ExpectUnknown(const Result& result, const std::string& arg) {
    EXPECT_EQ(result.exit_code, 64);
    EXPECT_TRUE(result.out.empty()) << result.out;
    const std::string line = "unknown argument: " + arg;
    ASSERT_EQ(result.err.find(line), 0u) << result.err;
    ASSERT_GT(result.err.size(), line.size());
    EXPECT_TRUE(result.err[line.size()] == '\r' || result.err[line.size()] == '\n');
    ExpectBanner(result.err);
    EXPECT_GT(result.err.find("Usage:"), line.size());
}

TEST(UsageArgs, NoArgumentsContinuesSilently) {
    ExpectContinue(RunUsage({}));
    ExpectContinue(RunUsage({L"helper.exe"}));
}

TEST(UsageArgs, ProgramNameIsNeverExamined) {
    for (const auto* name : {L"--help", L"-h", L"/?", L"--unknown", L""}) {
        ExpectContinue(RunUsage({name}));
        ExpectContinue(RunUsage({name, L"--parent-pid=1234"}));
    }
}

TEST(UsageArgs, EachHelpAliasWorksAtEveryPosition) {
    for (const auto* help : {L"--help", L"-h", L"/?"}) {
        for (int position = 1; position <= 3; ++position) {
            SCOPED_TRACE(::testing::Message() << help << " at " << position);
            std::vector<std::wstring> args{L"helper.exe", L"--parent-pid=1234",
                                           L"--pipe-name=x"};
            args.insert(args.begin() + position, help);
            ExpectHelp(RunUsage(args));
        }
    }
}

TEST(UsageArgs, UnknownArgumentStopsWithDiagnosticAndUsageOnStderr) {
    ExpectUnknown(RunUsage({L"helper.exe", L"--unknown"}), "--unknown");
    ExpectUnknown(RunUsage({L"helper.exe", L"--parent-pid=1234", L"--unknown"}),
                  "--unknown");
}

TEST(UsageArgs, ValueFlagPrefixesAcceptUnvalidatedSuffixes) {
    for (const auto* prefix : {L"--pipe-name=", L"--parent-pid=", L"--parent-handle="}) {
        for (const auto* value : {L"", L"1234", L"abc", L"x=y", L"--help"}) {
            const std::wstring arg = std::wstring(prefix) + value;
            SCOPED_TRACE(::testing::Message() << arg);
            ExpectContinue(RunUsage({L"helper.exe", arg}));
        }
    }
}

TEST(UsageArgs, LeviathanCallerArgumentsContinue) {
    ExpectContinue(RunUsage({L"leviathan-clipboard-helper.exe", L"--parent-pid=1234",
                        L"--parent-handle=5678"}));
}

TEST(UsageArgs, ShenCallerArgumentsWithBackslashesContinue) {
    ExpectContinue(RunUsage({L"leviathan-clipboard-helper.exe",
                        LR"(--pipe-name=\\.\pipe\leviathan-clipboard-shen-1234)",
                        L"--parent-pid=1234"}));
    // Also pin the single-leading-backslash spelling supplied in the contract.
    ExpectContinue(RunUsage({L"leviathan-clipboard-helper.exe",
                        LR"(--pipe-name=\.\pipe\leviathan-clipboard-shen-1234)",
                        L"--parent-pid=1234"}));
}

TEST(UsageArgs, NearMissFlagsAndPositionalsAreRejected) {
    for (const std::string arg : {"--pipe-name", "--parent-pid", "--parent-handle",
                                  "--PIPE-NAME=x", "-", "--", "positional",
                                  "--help=x", "--helpful", "-h=", "/?x", "--HELP",
                                  "--pipe-name-extra=x", ""}) {
        SCOPED_TRACE(arg);
        ExpectUnknown(RunUsage({L"helper.exe", std::wstring(arg.begin(), arg.end())}), arg);
    }
}

TEST(UsageArgs, SeparateValueArgumentIsRejected) {
    ExpectUnknown(RunUsage({L"helper.exe", L"--parent-pid", L"1234"}), "--parent-pid");
    ExpectUnknown(RunUsage({L"helper.exe", L"--parent-pid=", L"1234"}), "1234");
}

TEST(UsageArgs, HelpBeforeUnknownWins) {
    for (const auto* help : {L"--help", L"-h", L"/?"}) {
        ExpectHelp(RunUsage({L"helper.exe", L"--parent-pid=1234", help, L"--unknown"}));
    }
}

TEST(UsageArgs, UnknownBeforeHelpWins) {
    for (const auto* help : {L"--help", L"-h", L"/?"}) {
        ExpectUnknown(RunUsage({L"helper.exe", L"--parent-pid=1234", L"--unknown", help}),
                      "--unknown");
    }
}

}  // namespace
