// Pure-logic tests for the OLE-free helpers exposed via clipboard_ops.h:
//   * Utf8ToWide / WideToUtf8  (impl in string_utils.cpp)
//   * BuildCfHdropPayload      (impl in clipboard_format.cpp)
//   * OsClipboardHeldGuard     (impl in clipboard_format.cpp)
//
// These do not touch the real Win32 clipboard; the tests are safe to run
// in a non-interactive session (CI, no desktop).

#include "clipboard_ops.h"
#include "test_lite.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>     // DROPFILES

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using leviathan::clipboard_helper::BuildCfHdropPayload;
using leviathan::clipboard_helper::IsOsClipboardHeldByThisThread;
using leviathan::clipboard_helper::OsClipboardHeldGuard;
using leviathan::clipboard_helper::Utf8ToWide;
using leviathan::clipboard_helper::WideToUtf8;

// ─── Utf8ToWide / WideToUtf8 ──────────────────────────────────────────────

TEST(StringUtils, Utf8ToWideEmptyInputReturnsEmpty) {
    EXPECT_TRUE(Utf8ToWide("").empty());
}

TEST(StringUtils, WideToUtf8EmptyInputReturnsEmpty) {
    EXPECT_TRUE(WideToUtf8(L"").empty());
}

TEST(StringUtils, RoundTripsAsciiUnchanged) {
    const std::string in = "hello, world";
    const auto wide = Utf8ToWide(in);
    EXPECT_EQ(wide, std::wstring(L"hello, world"));
    EXPECT_EQ(WideToUtf8(wide), in);
}

namespace {

// Build a std::string from raw bytes without going through char escape
// sequences — clang-cl rejects \xE4 etc as "out of range" for signed
// char under /W4 /WX.
std::string MakeBytes(std::initializer_list<int> bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (int b : bytes) s.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    return s;
}

}  // namespace

TEST(StringUtils, RoundTripsBmpCharacters) {
    // CJK ideographs sit on the BMP; each UTF-8 char is 3 bytes, each
    // UTF-16 unit is one wchar_t. U+4F60 U+597D → 6 UTF-8 bytes, 2 wchar_ts.
    const std::string in = MakeBytes({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD});
    const auto wide = Utf8ToWide(in);
    ASSERT_EQ(wide.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<unsigned>(wide[0]), 0x4F60u);
    EXPECT_EQ(static_cast<unsigned>(wide[1]), 0x597Du);
    EXPECT_EQ(WideToUtf8(wide), in);
}

TEST(StringUtils, RoundTripsSupplementaryPlaneViaSurrogatePair) {
    // U+1F600 GRINNING FACE. UTF-8: F0 9F 98 80 (4 bytes). UTF-16: two
    // wchar_ts (high + low surrogate). The MultiByteToWideChar /
    // WideCharToMultiByte pair must preserve the codepoint across the
    // round trip — including the surrogate-pair encoding of UTF-16.
    const std::string in = MakeBytes({0xF0, 0x9F, 0x98, 0x80});
    const auto wide = Utf8ToWide(in);
    ASSERT_EQ(wide.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<unsigned>(wide[0]), 0xD83Du);  // high surrogate
    EXPECT_EQ(static_cast<unsigned>(wide[1]), 0xDE00u);  // low surrogate
    EXPECT_EQ(WideToUtf8(wide), in);
}

TEST(StringUtils, Utf8ToWideHandlesInvalidBytesGracefully) {
    // 0xFF is never a valid UTF-8 leading byte. We don't strictly need
    // the CRT's substitution behaviour — just that the call doesn't
    // crash and produces SOME wide string of reasonable length.
    const std::string in = MakeBytes({0xFF, 'm', 'i', 'd', 0xFF, 'e', 'n', 'd'});
    const auto wide = Utf8ToWide(in);
    EXPECT_GT(wide.size(), static_cast<std::size_t>(0));
}

// ─── BuildCfHdropPayload — layout ─────────────────────────────────────────

namespace {

const DROPFILES* HeaderOf(const std::vector<std::uint8_t>& buf) {
    return reinterpret_cast<const DROPFILES*>(buf.data());
}

const wchar_t* PathDataOf(const std::vector<std::uint8_t>& buf) {
    return reinterpret_cast<const wchar_t*>(buf.data() + sizeof(DROPFILES));
}

}  // namespace

TEST(BuildCfHdrop, EmptyPathListReturnsEmptyBuffer) {
    // CF_HDROP with zero paths is not a meaningful payload; the helper
    // refuses up front rather than emitting a DROPFILES header that
    // points at a single trailing NUL.
    EXPECT_TRUE(BuildCfHdropPayload({}).empty());
}

TEST(BuildCfHdrop, SinglePathHeaderShape) {
    const std::vector<std::wstring> paths = {L"C:\\one.txt"};
    const auto buf = BuildCfHdropPayload(paths);
    ASSERT_TRUE(buf.size() > sizeof(DROPFILES));

    const DROPFILES* df = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(df->pFiles), static_cast<unsigned>(sizeof(DROPFILES)));
    EXPECT_TRUE(df->fWide);
    EXPECT_EQ(static_cast<int>(df->fNC), 0);
    EXPECT_EQ(df->pt.x, 0);
    EXPECT_EQ(df->pt.y, 0);
}

TEST(BuildCfHdrop, SinglePathByteSize) {
    const std::wstring path = L"C:\\one.txt";
    const std::vector<std::wstring> paths = {path};
    const auto buf = BuildCfHdropPayload(paths);
    // Header + path chars + path-NUL + list-terminator-NUL.
    const std::size_t expected = sizeof(DROPFILES)
                                 + (path.size() + 1 + 1) * sizeof(wchar_t);
    EXPECT_EQ(buf.size(), expected);
}

TEST(BuildCfHdrop, SinglePathHasDoubleNulTerminator) {
    const std::wstring path = L"a.txt";
    const auto buf = BuildCfHdropPayload({path});
    const wchar_t* p = PathDataOf(buf);

    // First path chars are intact.
    for (std::size_t i = 0; i < path.size(); ++i) {
        EXPECT_EQ(p[i], path[i]);
    }
    // Then per-path NUL, then list-terminator NUL.
    EXPECT_EQ(p[path.size()], L'\0');
    EXPECT_EQ(p[path.size() + 1], L'\0');
}

TEST(BuildCfHdrop, MultiplePathsPreserveOrder) {
    const std::vector<std::wstring> paths = {
        L"C:\\a\\one.txt",
        L"C:\\a\\two.bin",
        L"D:\\three.dat",
    };
    const auto buf = BuildCfHdropPayload(paths);
    const wchar_t* p = PathDataOf(buf);

    for (const auto& expected : paths) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(*p, expected[i]);
            ++p;
        }
        EXPECT_EQ(*p, L'\0');
        ++p;
    }
    // Final list-terminator NUL.
    EXPECT_EQ(*p, L'\0');
}

TEST(BuildCfHdrop, MultiplePathsByteSize) {
    const std::vector<std::wstring> paths = {L"ab", L"cdef", L"g"};
    const auto buf = BuildCfHdropPayload(paths);
    const std::size_t wchar_total = (2 + 1) + (4 + 1) + (1 + 1) + 1;
    EXPECT_EQ(buf.size(), sizeof(DROPFILES) + wchar_total * sizeof(wchar_t));
}

TEST(BuildCfHdrop, PathContentsHonorWchar) {
    // Non-ASCII wide chars should land verbatim in the UTF-16 payload
    // — BuildCfHdropPayload must not coerce to ASCII.
    const std::wstring path = L"你好.txt";  // 你好.txt
    const auto buf = BuildCfHdropPayload({path});
    const wchar_t* p = PathDataOf(buf);
    EXPECT_EQ(static_cast<unsigned>(p[0]), 0x4F60u);
    EXPECT_EQ(static_cast<unsigned>(p[1]), 0x597Du);
    EXPECT_EQ(p[2], L'.');
    EXPECT_EQ(p[3], L't');
    EXPECT_EQ(p[4], L'x');
    EXPECT_EQ(p[5], L't');
    EXPECT_EQ(p[6], L'\0');
    EXPECT_EQ(p[7], L'\0');
}

TEST(BuildCfHdrop, EmptyStringPathRespectsLayoutInvariants) {
    // An empty path is degenerate but not invalid — it produces one
    // per-path NUL followed immediately by the list-terminator NUL,
    // i.e. the same two-NUL sequence as the trailing tail. The function
    // must still emit the DROPFILES header and the double-NUL.
    const auto buf = BuildCfHdropPayload({std::wstring()});
    ASSERT_EQ(buf.size(), sizeof(DROPFILES) + 2 * sizeof(wchar_t));
    const wchar_t* p = PathDataOf(buf);
    EXPECT_EQ(p[0], L'\0');
    EXPECT_EQ(p[1], L'\0');
}

// ─── OsClipboardHeldGuard ─────────────────────────────────────────────────

TEST(OsClipboardHeldGuard, NotHeldByDefault) {
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
}

TEST(OsClipboardHeldGuard, EnteredScopeHoldsTheClipboard) {
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
    {
        OsClipboardHeldGuard guard;
        EXPECT_TRUE(IsOsClipboardHeldByThisThread());
    }
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
}

TEST(OsClipboardHeldGuard, NestedGuardsCountSymmetrically) {
    // The guard is reference-counted, not boolean — WM_RENDERALLFORMATS
    // dispatches OnRenderFormat for each advertised format and the
    // counter must roll back exactly when the outer scope exits.
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
    {
        OsClipboardHeldGuard outer;
        EXPECT_TRUE(IsOsClipboardHeldByThisThread());
        {
            OsClipboardHeldGuard inner;
            EXPECT_TRUE(IsOsClipboardHeldByThisThread());
        }
        EXPECT_TRUE(IsOsClipboardHeldByThisThread());
    }
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
}

TEST(OsClipboardHeldGuard, GuardStateIsThreadLocal) {
    // Sibling thread must NOT see this thread's guard. The STA worker's
    // re-entrancy guard would corrupt unrelated pipe-thread state if
    // this was a process-global flag.
    OsClipboardHeldGuard guard;
    ASSERT_TRUE(IsOsClipboardHeldByThisThread());

    bool sibling_saw_held = true;
    std::thread sibling([&]() {
        sibling_saw_held = IsOsClipboardHeldByThisThread();
    });
    sibling.join();
    EXPECT_FALSE(sibling_saw_held);
}

TEST(OsClipboardHeldGuard, SiblingThreadsCanGuardIndependently) {
    // Threads do not contend on the guard — a producer thread that holds
    // its own guard does not affect the test thread's view, and vice
    // versa. Locks this contract so a future "optimise" to a shared
    // atomic counter would fail visibly.
    std::thread sibling([]() {
        OsClipboardHeldGuard guard;
        EXPECT_TRUE(IsOsClipboardHeldByThisThread());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
    sibling.join();
    EXPECT_FALSE(IsOsClipboardHeldByThisThread());
}
