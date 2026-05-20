// Pure-logic tests for BuildFileGroupDescriptorPayload — the
// CFSTR_FILEDESCRIPTORW byte builder shared with the IDataObject server.
// Tests deliberately do NOT instantiate the IDataObject itself; that
// needs a real COM apartment and is exercised manually.

#include "clipboard_ops.h"
#include "test_lite.h"
#include "virtual_file_spec.h"

#include <windows.h>
#include <shlobj.h>     // FILEGROUPDESCRIPTORW, FILEDESCRIPTORW, FD_*, FILE_ATTRIBUTE_*

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using leviathan::clipboard_helper::BuildFileGroupDescriptorPayload;
using leviathan::clipboard_helper::kMaxVirtualFileSpecs;
using leviathan::clipboard_helper::VirtualFileSpec;

namespace {

const FILEGROUPDESCRIPTORW* HeaderOf(const std::vector<std::uint8_t>& buf) {
    return reinterpret_cast<const FILEGROUPDESCRIPTORW*>(buf.data());
}

VirtualFileSpec MakeFileSpec(std::wstring name, std::uint64_t size,
                             std::string file_id = "id",
                             bool is_directory = false) {
    VirtualFileSpec s;
    s.name         = std::move(name);
    s.size         = size;
    s.file_id      = std::move(file_id);
    s.is_directory = is_directory;
    return s;
}

}  // namespace

// ─── Reject cases ─────────────────────────────────────────────────────────

TEST(BuildFileGroupDescriptor, EmptySpecsReturnsEmpty) {
    EXPECT_TRUE(BuildFileGroupDescriptorPayload({}).empty());
}

TEST(BuildFileGroupDescriptor, RejectsOverCap) {
    std::vector<VirtualFileSpec> too_many(kMaxVirtualFileSpecs + 1,
                                          MakeFileSpec(L"x", 0));
    EXPECT_TRUE(BuildFileGroupDescriptorPayload(too_many).empty());
}

// ─── Header / size ────────────────────────────────────────────────────────

TEST(BuildFileGroupDescriptor, SingleFileBufferSize) {
    // FILEGROUPDESCRIPTORW already embeds one FILEDESCRIPTORW slot.
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"a.txt", 0)});
    EXPECT_EQ(buf.size(), sizeof(FILEGROUPDESCRIPTORW));
}

TEST(BuildFileGroupDescriptor, MultipleFilesBufferSize) {
    // N entries → base + (N-1) extra FILEDESCRIPTORW slots.
    const std::size_t kCount = 5;
    std::vector<VirtualFileSpec> specs(kCount, MakeFileSpec(L"x", 0));
    const auto buf = BuildFileGroupDescriptorPayload(specs);
    EXPECT_EQ(buf.size(),
              sizeof(FILEGROUPDESCRIPTORW) + (kCount - 1) * sizeof(FILEDESCRIPTORW));
}

TEST(BuildFileGroupDescriptor, CItemsMatchesSpecCount) {
    const auto buf = BuildFileGroupDescriptorPayload({
        MakeFileSpec(L"a", 0),
        MakeFileSpec(L"b", 0),
        MakeFileSpec(L"c", 0),
    });
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->cItems), static_cast<unsigned>(3));
}

// ─── Per-entry flags / attributes ─────────────────────────────────────────

TEST(BuildFileGroupDescriptor, DefaultFlagsCoverSizeAttributesAndProgressUI) {
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"a.txt", 0)});
    const auto* fg = HeaderOf(buf);
    // FreeRDP-minimal flag set: FD_FILESIZE | FD_ATTRIBUTES | FD_WRITESTIME
    // | FD_PROGRESSUI.  FD_CREATETIME was dropped 2026-05-20 to mimic
    // rdpclip/wf_cliprdr.c on Win11.
    const DWORD expected = FD_FILESIZE | FD_ATTRIBUTES | FD_WRITESTIME | FD_PROGRESSUI;
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].dwFlags),
              static_cast<unsigned>(expected));
}

TEST(BuildFileGroupDescriptor, FileEntryHasNormalAttribute) {
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"a.txt", 0, "f0", /*is_directory=*/false)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].dwFileAttributes),
              static_cast<unsigned>(FILE_ATTRIBUTE_NORMAL));
}

TEST(BuildFileGroupDescriptor, DirectoryEntryHasDirectoryAttribute) {
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"folder", 0, "d0", /*is_directory=*/true)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].dwFileAttributes),
              static_cast<unsigned>(FILE_ATTRIBUTE_DIRECTORY));
}

// ─── 64-bit size split ────────────────────────────────────────────────────

TEST(BuildFileGroupDescriptor, SmallSizeFitsInLowWord) {
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"a", 4096)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeLow),
              static_cast<unsigned>(4096));
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeHigh), 0u);
}

TEST(BuildFileGroupDescriptor, SizeAtFourGigBoundaryRollsIntoHighWord) {
    // 2^32 = 0x1_0000_0000 → Low=0, High=1.
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"big", 0x100000000ull)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeLow),  0u);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeHigh), 1u);
}

TEST(BuildFileGroupDescriptor, LargeSizeSplitsAcrossLowAndHigh) {
    // 0xDEADBEEF_CAFEBABE — exercise both halves with distinct nibbles.
    const std::uint64_t size = 0xDEADBEEFCAFEBABEull;
    const auto buf = BuildFileGroupDescriptorPayload(
        {MakeFileSpec(L"chimera", size)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeLow),  0xCAFEBABEu);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeHigh), 0xDEADBEEFu);
}

// ─── cFileName population ────────────────────────────────────────────────

TEST(BuildFileGroupDescriptor, ShortNameCopiedAsIsWithNulTerminator) {
    const std::wstring name = L"hello.bin";
    const auto buf = BuildFileGroupDescriptorPayload({MakeFileSpec(name, 0)});
    const auto* fg = HeaderOf(buf);
    for (std::size_t i = 0; i < name.size(); ++i) {
        EXPECT_EQ(fg->fgd[0].cFileName[i], name[i]);
    }
    EXPECT_EQ(fg->fgd[0].cFileName[name.size()], L'\0');
}

TEST(BuildFileGroupDescriptor, ForwardSlashNormalizedToBackslash) {
    // Cross-platform announcements arrive with '/' separators; the
    // shell virtual-file consumers historically only honor '\\' for
    // rebuilt directory trees inside the drop target.
    const std::wstring name = L"sub/dir/leaf.txt";
    const auto buf = BuildFileGroupDescriptorPayload({MakeFileSpec(name, 0)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(fg->fgd[0].cFileName[3],  L'\\');
    EXPECT_EQ(fg->fgd[0].cFileName[7],  L'\\');
    EXPECT_EQ(fg->fgd[0].cFileName[name.size()], L'\0');
}

TEST(BuildFileGroupDescriptor, NameAtMaxPathBoundaryFits) {
    // A name of exactly MAX_PATH - 1 wchars fits with a trailing NUL at
    // index MAX_PATH - 1. No truncation needed.
    const std::wstring name(MAX_PATH - 1, L'a');
    const auto buf = BuildFileGroupDescriptorPayload({MakeFileSpec(name, 0)});
    const auto* fg = HeaderOf(buf);
    for (std::size_t i = 0; i < name.size(); ++i) {
        ASSERT_EQ(fg->fgd[0].cFileName[i], L'a');
    }
    EXPECT_EQ(fg->fgd[0].cFileName[MAX_PATH - 1], L'\0');
}

TEST(BuildFileGroupDescriptor, OverlongNameTruncatedToMaxPathMinusOne) {
    // Names exceeding MAX_PATH - 1 wchars get truncated with NUL at
    // index MAX_PATH - 1.
    std::wstring name(MAX_PATH + 10, L'b');
    const auto buf = BuildFileGroupDescriptorPayload({MakeFileSpec(name, 0)});
    const auto* fg = HeaderOf(buf);
    // First MAX_PATH-1 chars preserved.
    for (std::size_t i = 0; i < static_cast<std::size_t>(MAX_PATH - 1); ++i) {
        ASSERT_EQ(fg->fgd[0].cFileName[i], L'b');
    }
    EXPECT_EQ(fg->fgd[0].cFileName[MAX_PATH - 1], L'\0');
}

TEST(BuildFileGroupDescriptor, TruncationBacksOffOnHighSurrogateSplit) {
    // Construct a string where index (MAX_PATH - 2) holds the high
    // surrogate of a pair and (MAX_PATH - 1) holds the low. A naive
    // truncation at MAX_PATH - 1 would leave the high surrogate
    // dangling. The helper must back off by one wchar.
    std::wstring name;
    name.resize(MAX_PATH - 2, L'c');
    name.push_back(static_cast<wchar_t>(0xD83D));  // high surrogate (U+1F600 part 1)
    name.push_back(static_cast<wchar_t>(0xDE00));  // low  surrogate (U+1F600 part 2)
    // name.size() == MAX_PATH; copy will start at MAX_PATH-1, hit the
    // high surrogate at copy-1, and back off to MAX_PATH-2.
    const auto buf = BuildFileGroupDescriptorPayload({MakeFileSpec(name, 0)});
    const auto* fg = HeaderOf(buf);
    EXPECT_EQ(fg->fgd[0].cFileName[MAX_PATH - 2], L'\0');
    EXPECT_EQ(fg->fgd[0].cFileName[MAX_PATH - 3], L'c');
}

// ─── Per-entry independence ───────────────────────────────────────────────

TEST(BuildFileGroupDescriptor, EachEntryPopulatedIndependently) {
    // Build a small mixed set and verify each fgd[i] reflects its
    // matching spec — common refactor bug is to copy fgd[0] into
    // every slot.
    const auto buf = BuildFileGroupDescriptorPayload({
        MakeFileSpec(L"alpha.txt", 1, "0", /*is_directory=*/false),
        MakeFileSpec(L"beta",      0, "1", /*is_directory=*/true),
        MakeFileSpec(L"gamma.bin", 0xDEADBEEFCAFEBABEull, "2"),
    });
    const auto* fg = HeaderOf(buf);

    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeLow), 1u);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].dwFileAttributes),
              static_cast<unsigned>(FILE_ATTRIBUTE_NORMAL));
    EXPECT_EQ(fg->fgd[0].cFileName[0], L'a');

    EXPECT_EQ(static_cast<unsigned>(fg->fgd[1].dwFileAttributes),
              static_cast<unsigned>(FILE_ATTRIBUTE_DIRECTORY));
    EXPECT_EQ(fg->fgd[1].cFileName[0], L'b');

    EXPECT_EQ(static_cast<unsigned>(fg->fgd[2].nFileSizeLow),  0xCAFEBABEu);
    EXPECT_EQ(static_cast<unsigned>(fg->fgd[2].nFileSizeHigh), 0xDEADBEEFu);
    EXPECT_EQ(fg->fgd[2].cFileName[0], L'g');
}
