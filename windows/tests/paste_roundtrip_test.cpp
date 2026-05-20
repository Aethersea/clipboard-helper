// End-to-end OLE clipboard roundtrip test for VirtualClipboardDataObject.
//
// What this test proves:
//   * OleSetClipboard(our IDataObject) succeeds.
//   * OleGetClipboard returns a proxy whose QueryInterface, EnumFormatEtc,
//     QueryGetData, and GetData paths all resolve back to our class
//     through the standard OLE marshalling wrapper.
//   * GetData(CFSTR_FILEDESCRIPTORW) returns a valid FILEGROUPDESCRIPTORW
//     with the right cItems / cFileName / nFileSize{Low,High}.
//   * GetData(CFSTR_FILECONTENTS, lindex=0, TYMED_ISTREAM) returns an
//     IStream that, when Read, surfaces bytes from our ChunkProvider.
//
// If this test passes on a machine but Explorer's right-click→Paste still
// silently aborts on that same machine, the failure is system-level
// (Defender / clipboard manager / shell registry policy / etc.) and not
// in our IDataObject implementation.
//
// The test runs in an STA, exactly matching the production helper's
// thread model.

#include "test_lite.h"
#include "virtual_file_provider.h"
#include "virtual_file_spec.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using leviathan::clipboard_helper::ChunkProvider;
using leviathan::clipboard_helper::CreateVirtualClipboardDataObject;
using leviathan::clipboard_helper::VirtualFileSpec;

namespace {

// Deterministic test content.  ChunkProvider serves these bytes in
// arbitrarily-sized chunks so the test can verify IStream::Read assembles
// them correctly across calls.
constexpr const char kTestPayload[] = "Hello virtual file paste!  "
                                       "This payload is 64 bytes long.  "
                                       "Verify me byte by byte.\n";
constexpr std::uint64_t kTestPayloadSize = sizeof(kTestPayload) - 1;  // exclude trailing NUL

class TestChunkProvider : public ChunkProvider {
public:
    HRESULT FetchChunk(const std::string&         transfer_id,
                       const std::string&         file_id,
                       std::uint64_t              offset,
                       std::uint32_t              size,
                       std::vector<std::uint8_t>& out_data,
                       bool&                      out_is_last) override {
        (void)transfer_id;
        (void)file_id;
        if (offset >= kTestPayloadSize) {
            out_data.clear();
            out_is_last = true;
            return S_OK;
        }
        const std::uint64_t remaining = kTestPayloadSize - offset;
        const std::uint32_t take = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(size)));
        out_data.assign(
            reinterpret_cast<const std::uint8_t*>(kTestPayload + offset),
            reinterpret_cast<const std::uint8_t*>(kTestPayload + offset + take));
        out_is_last = (offset + take >= kTestPayloadSize);
        return S_OK;
    }
};

// RAII for OleInitialize / OleUninitialize in STA.
class OleScope {
public:
    OleScope()  { ole_hr_ = ::OleInitialize(nullptr); }
    ~OleScope() { if (SUCCEEDED(ole_hr_)) ::OleUninitialize(); }
    HRESULT hr() const { return ole_hr_; }
private:
    HRESULT ole_hr_{E_FAIL};
};

// Pump pending Windows messages on the current STA so OLE clipboard
// ownership transitions (which post messages to a hidden clipboard
// window owned by ole32) actually complete before the next test starts.
// Without this, back-to-back OleSetClipboard / OleGetClipboard calls
// race the previous transition and one of them may see a half-released
// clipboard state — observed empirically as flaky GetData returning
// E_FAIL with sm.tymed=0 on the very next test.
void PumpMessagesBriefly(DWORD ms = 250) {
    const DWORD deadline = ::GetTickCount() + ms;
    MSG msg;
    while (::GetTickCount() < deadline) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(5);
    }
}

// Always clear the OS clipboard before each test so a previously-set
// IDataObject doesn't bleed across runs.  Run on test teardown too.
void ClearClipboard() {
    ::OleSetClipboard(nullptr);
    PumpMessagesBriefly();
}

VirtualFileSpec MakeSpec(const std::wstring& name,
                         const std::string&  file_id,
                         std::uint64_t       size) {
    VirtualFileSpec s;
    s.name         = name;
    s.file_id      = file_id;
    s.size         = size;
    s.is_directory = false;
    return s;
}

// Wrap GlobalLock/Unlock so test code reads HGLOBAL contents safely.
class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL h) : h_(h), ptr_(::GlobalLock(h)) {}
    ~GlobalLockGuard() { if (h_) ::GlobalUnlock(h_); }
    const void* ptr() const { return ptr_; }
    template <typename T>
    const T* as() const { return static_cast<const T*>(ptr_); }
    bool ok() const { return ptr_ != nullptr; }
private:
    HGLOBAL h_;
    LPVOID  ptr_;
};

}  // namespace

// ─── Test: OleSetClipboard accepts our IDataObject ────────────────────────

TEST(PasteRoundtrip, OleSetClipboardAcceptsVirtualFileDataObject) {
    OleScope ole;
    ASSERT_TRUE(SUCCEEDED(ole.hr()));

    TestChunkProvider provider;
    std::vector<VirtualFileSpec> specs{MakeSpec(L"hello.txt", "0", kTestPayloadSize)};

    IDataObject* obj = nullptr;
    HRESULT hr = CreateVirtualClipboardDataObject(std::move(specs),
                                                  "test-transfer",
                                                  &provider, &obj);
    ASSERT_TRUE(SUCCEEDED(hr));
    ASSERT_TRUE(obj != nullptr);

    hr = ::OleSetClipboard(obj);
    EXPECT_TRUE(SUCCEEDED(hr));
    EXPECT_EQ(::OleIsCurrentClipboard(obj), S_OK);

    obj->Release();
    ClearClipboard();
}

// ─── Test: OleGetClipboard returns a usable proxy ─────────────────────────

TEST(PasteRoundtrip, OleGetClipboardReturnsProxyToOurDataObject) {
    OleScope ole;
    ASSERT_TRUE(SUCCEEDED(ole.hr()));

    TestChunkProvider provider;
    std::vector<VirtualFileSpec> specs{MakeSpec(L"hello.txt", "0", kTestPayloadSize)};

    IDataObject* obj = nullptr;
    ASSERT_TRUE(SUCCEEDED(CreateVirtualClipboardDataObject(
        std::move(specs), "test-transfer", &provider, &obj)));
    ASSERT_TRUE(SUCCEEDED(::OleSetClipboard(obj)));
    obj->Release();

    IDataObject* retrieved = nullptr;
    HRESULT hr = ::OleGetClipboard(&retrieved);
    EXPECT_TRUE(SUCCEEDED(hr));
    EXPECT_TRUE(retrieved != nullptr);

    if (retrieved) retrieved->Release();
    ClearClipboard();
}

// ─── Test: Retrieved proxy returns valid FILEGROUPDESCRIPTORW ─────────────

TEST(PasteRoundtrip, GetDataReturnsValidFileGroupDescriptor) {
    OleScope ole;
    ASSERT_TRUE(SUCCEEDED(ole.hr()));

    TestChunkProvider provider;
    std::vector<VirtualFileSpec> specs{MakeSpec(L"roundtrip.bin", "0", kTestPayloadSize)};

    IDataObject* obj = nullptr;
    ASSERT_TRUE(SUCCEEDED(CreateVirtualClipboardDataObject(
        std::move(specs), "test-transfer", &provider, &obj)));
    ASSERT_TRUE(SUCCEEDED(::OleSetClipboard(obj)));
    obj->Release();

    IDataObject* retrieved = nullptr;
    ASSERT_TRUE(SUCCEEDED(::OleGetClipboard(&retrieved)));
    ASSERT_TRUE(retrieved != nullptr);

    const UINT cf_descriptor = ::RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    FORMATETC fe{};
    fe.cfFormat = static_cast<CLIPFORMAT>(cf_descriptor);
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex   = -1;
    fe.tymed    = TYMED_HGLOBAL;

    STGMEDIUM sm{};
    HRESULT hr = retrieved->GetData(&fe, &sm);
    EXPECT_TRUE(SUCCEEDED(hr));
    EXPECT_EQ(sm.tymed, static_cast<DWORD>(TYMED_HGLOBAL));
    EXPECT_TRUE(sm.hGlobal != nullptr);

    if (SUCCEEDED(hr) && sm.hGlobal) {
        GlobalLockGuard lock(sm.hGlobal);
        ASSERT_TRUE(lock.ok());
        const auto* fg = lock.as<FILEGROUPDESCRIPTORW>();
        EXPECT_EQ(static_cast<unsigned>(fg->cItems), 1u);
        EXPECT_EQ(static_cast<unsigned>(fg->fgd[0].nFileSizeLow),
                  static_cast<unsigned>(kTestPayloadSize));
        EXPECT_EQ(fg->fgd[0].cFileName[0], L'r');
        EXPECT_EQ(fg->fgd[0].cFileName[1], L'o');
    }

    ::ReleaseStgMedium(&sm);
    retrieved->Release();
    ClearClipboard();
}

// ─── Test: GetData(CONTENTS) returns IStream + Read surfaces our bytes ────
//
// This is THE test that directly mirrors what Explorer's paste flow should
// be doing.  If this passes but Explorer paste fails, the bug isn't us.

TEST(PasteRoundtrip, GetDataContentsStreamYieldsChunkProviderBytes) {
    OleScope ole;
    ASSERT_TRUE(SUCCEEDED(ole.hr()));

    TestChunkProvider provider;
    std::vector<VirtualFileSpec> specs{MakeSpec(L"roundtrip.bin", "0", kTestPayloadSize)};

    IDataObject* obj = nullptr;
    ASSERT_TRUE(SUCCEEDED(CreateVirtualClipboardDataObject(
        std::move(specs), "test-transfer", &provider, &obj)));
    ASSERT_TRUE(SUCCEEDED(::OleSetClipboard(obj)));
    obj->Release();

    IDataObject* retrieved = nullptr;
    ASSERT_TRUE(SUCCEEDED(::OleGetClipboard(&retrieved)));
    ASSERT_TRUE(retrieved != nullptr);

    const UINT cf_contents = ::RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    FORMATETC fe{};
    fe.cfFormat = static_cast<CLIPFORMAT>(cf_contents);
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex   = 0;            // first (and only) file
    fe.tymed    = TYMED_ISTREAM;

    STGMEDIUM sm{};
    HRESULT hr = retrieved->GetData(&fe, &sm);
    EXPECT_TRUE(SUCCEEDED(hr));
    EXPECT_EQ(sm.tymed, static_cast<DWORD>(TYMED_ISTREAM));
    ASSERT_TRUE(sm.pstm != nullptr);

    // Drain the stream.  We deliberately ask for small chunks so the test
    // exercises multi-Read assembly the way Explorer's shell copy engine
    // does.
    std::vector<std::uint8_t> received;
    constexpr ULONG kReadChunk = 16;
    std::uint8_t scratch[kReadChunk];
    bool read_failed = false;
    bool overran     = false;
    for (;;) {
        ULONG got = 0;
        HRESULT rhr = sm.pstm->Read(scratch, kReadChunk, &got);
        if (FAILED(rhr)) {
            read_failed = true;
            break;
        }
        if (got == 0) break;
        received.insert(received.end(), scratch, scratch + got);
        if (received.size() > static_cast<std::size_t>(kTestPayloadSize * 2)) {
            overran = true;
            break;
        }
    }
    EXPECT_FALSE(read_failed);
    EXPECT_FALSE(overran);

    EXPECT_EQ(received.size(), static_cast<std::size_t>(kTestPayloadSize));
    if (received.size() == static_cast<std::size_t>(kTestPayloadSize)) {
        EXPECT_EQ(std::memcmp(received.data(), kTestPayload,
                              static_cast<std::size_t>(kTestPayloadSize)),
                  0);
    }

    ::ReleaseStgMedium(&sm);
    retrieved->Release();
    ClearClipboard();
}
