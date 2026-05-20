// paste_to_disk.exe — diagnostic tool that does the full paste-to-disk
// pipeline for a virtual-file IDataObject, WITHOUT involving Explorer.
//
// Two modes:
//
//   paste_to_disk.exe --self-test [<destination_folder>]
//       Self-contained mode: publishes a known virtual-file IDataObject
//       to the OS clipboard, reads it back via OleGetClipboard, walks the
//       FILEGROUPDESCRIPTORW, fetches each CFSTR_FILECONTENTS IStream,
//       writes the bytes to disk, and verifies the file content matches
//       the expected payload.  No leviathan / shen / Explorer involvement
//       — proves the full publish→clipboard→read→IStream→disk-write
//       chain works on this machine.
//       Default destination: a fresh temp dir under %TEMP%.
//
//   paste_to_disk.exe <destination_folder>
//       Reads whatever's currently on the OS clipboard and pastes any
//       virtual files to <destination_folder>.  Use this after copying
//       from shen → run this in place of Explorer's right-click → Paste.
//
// Exit code 0 = success.  Non-zero on failure (see code).
//
// Build target: clipboard_helper_paste_to_disk (see tests/CMakeLists.txt).

#include "virtual_file_provider.h"
#include "virtual_file_spec.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using leviathan::clipboard_helper::ChunkProvider;
using leviathan::clipboard_helper::CreateVirtualClipboardDataObject;
using leviathan::clipboard_helper::VirtualFileSpec;

// ─── Deterministic test payload (self-test mode) ───────────────────────────

constexpr const char kSelfTestFile1Content[] =
    "Hello virtual file paste — file 1.\n"
    "This text proves the full publish → OleGetClipboard → "
    "GetData(CFSTR_FILECONTENTS) → IStream::Read → disk-write chain.\n"
    "If you can see this file on disk after running --self-test, our "
    "IDataObject is functionally correct.\n";
constexpr const char kSelfTestFile2Content[] =
    "File 2 — a smaller payload to exercise multi-file descriptors.\n";

constexpr const wchar_t kSelfTestFile1Name[] = L"paste-to-disk-test-1.txt";
constexpr const wchar_t kSelfTestFile2Name[] = L"paste-to-disk-test-2.txt";

struct SelfTestFile {
    std::wstring  name;
    const char*   content;
    std::uint64_t size;
};

std::vector<SelfTestFile> SelfTestFiles() {
    return {
        {kSelfTestFile1Name, kSelfTestFile1Content, sizeof(kSelfTestFile1Content) - 1},
        {kSelfTestFile2Name, kSelfTestFile2Content, sizeof(kSelfTestFile2Content) - 1},
    };
}

// ChunkProvider that serves bytes from an in-memory map (file_id → payload).
class SelfTestChunkProvider : public ChunkProvider {
public:
    void Add(const std::string& file_id, const char* data, std::uint64_t size) {
        payloads_[file_id] = {data, size};
    }
    HRESULT FetchChunk(const std::string&         /*transfer_id*/,
                       const std::string&         file_id,
                       std::uint64_t              offset,
                       std::uint32_t              size,
                       std::vector<std::uint8_t>& out_data,
                       bool&                      out_is_last) override {
        auto it = payloads_.find(file_id);
        if (it == payloads_.end()) {
            out_data.clear();
            out_is_last = true;
            return E_FAIL;
        }
        const Payload& p = it->second;
        if (offset >= p.size) {
            out_data.clear();
            out_is_last = true;
            return S_OK;
        }
        const std::uint64_t remaining = p.size - offset;
        const std::uint32_t take = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(size)));
        out_data.assign(
            reinterpret_cast<const std::uint8_t*>(p.data + offset),
            reinterpret_cast<const std::uint8_t*>(p.data + offset + take));
        out_is_last = (offset + take >= p.size);
        return S_OK;
    }
private:
    struct Payload {
        const char*   data;
        std::uint64_t size;
    };
    std::map<std::string, Payload> payloads_;

    // map header included on demand to avoid bloating includes
    // (kept inline so the compile error is visible if missing).
public:
    // Public typedef to make the unordered_map cheap to include.
};

// ─── Misc utilities ────────────────────────────────────────────────────────

struct ScopedOle {
    HRESULT hr;
    ScopedOle()  : hr(::OleInitialize(nullptr)) {}
    ~ScopedOle() { if (SUCCEEDED(hr)) ::OleUninitialize(); }
};

std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1];
    const DWORD len = ::GetTempPathW(MAX_PATH, buf);
    if (len == 0) return L".";
    return std::wstring(buf, len);
}

std::wstring MakeFreshSubdir(const std::wstring& parent) {
    // Pick a directory name based on system time so successive runs don't
    // collide.  Best-effort; if creation fails, fall back to parent.
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    wchar_t name[64];
    std::swprintf(name, sizeof(name) / sizeof(name[0]),
                  L"paste-to-disk-%08lX%08lX",
                  static_cast<unsigned long>(ft.dwHighDateTime),
                  static_cast<unsigned long>(ft.dwLowDateTime));
    std::wstring dir = parent;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    dir += name;
    if (!::CreateDirectoryW(dir.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return parent;
    }
    return dir;
}

// ─── Paste-from-clipboard core ─────────────────────────────────────────────

struct PasteResult {
    int          files_seen{0};
    int          files_written{0};
    int          failures{0};
    // Captured per-file: filename + actual bytes written, for self-test
    // verification.
    std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> contents;
};

bool WriteStreamToFile(IStream* stream, const std::wstring& full_path,
                       std::uint64_t expected_size,
                       std::vector<std::uint8_t>* out_capture) {
    HANDLE fh = ::CreateFileW(full_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"  CreateFileW failed (GLE=%lu): %ls\n",
                      ::GetLastError(), full_path.c_str());
        return false;
    }
    bool ok = true;
    std::uint64_t total = 0;
    std::uint8_t  buf[64 * 1024];
    for (;;) {
        ULONG got = 0;
        const HRESULT rhr = stream->Read(buf, sizeof(buf), &got);
        if (FAILED(rhr)) {
            std::fwprintf(stderr,
                          L"  IStream::Read failed: hr=0x%08lx pos=%llu\n",
                          static_cast<unsigned long>(rhr),
                          static_cast<unsigned long long>(total));
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD written = 0;
        if (!::WriteFile(fh, buf, got, &written, nullptr) || written != got) {
            std::fwprintf(stderr,
                          L"  WriteFile short/failed: GLE=%lu wrote=%lu of %lu\n",
                          ::GetLastError(), written, got);
            ok = false;
            break;
        }
        if (out_capture) {
            out_capture->insert(out_capture->end(), buf, buf + got);
        }
        total += got;
        if (expected_size > 0 && total > expected_size * 4) {
            std::fwprintf(stderr,
                          L"  ERROR: stream produced >4x expected size; aborting\n");
            ok = false;
            break;
        }
    }
    ::CloseHandle(fh);
    if (ok) {
        std::fwprintf(stdout,
                      L"  wrote %llu bytes → %ls\n",
                      static_cast<unsigned long long>(total),
                      full_path.c_str());
    }
    return ok;
}

PasteResult PasteFromClipboardToFolder(const std::wstring& dest_dir,
                                       bool capture_content) {
    PasteResult result;

    IDataObject* obj = nullptr;
    const HRESULT hr = ::OleGetClipboard(&obj);
    if (FAILED(hr) || obj == nullptr) {
        std::fwprintf(stderr,
                      L"OleGetClipboard failed: hr=0x%08lx (clipboard empty / no IDataObject?)\n",
                      static_cast<unsigned long>(hr));
        result.failures = 1;
        return result;
    }
    std::fwprintf(stdout, L"OleGetClipboard: got IDataObject proxy.\n");

    const UINT cf_descriptor = ::RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    const UINT cf_contents   = ::RegisterClipboardFormatW(CFSTR_FILECONTENTS);

    FORMATETC fe_desc{};
    fe_desc.cfFormat = static_cast<CLIPFORMAT>(cf_descriptor);
    fe_desc.dwAspect = DVASPECT_CONTENT;
    fe_desc.lindex   = -1;
    fe_desc.tymed    = TYMED_HGLOBAL;

    STGMEDIUM sm_desc{};
    const HRESULT g_hr = obj->GetData(&fe_desc, &sm_desc);
    if (FAILED(g_hr) || sm_desc.tymed != TYMED_HGLOBAL || sm_desc.hGlobal == nullptr) {
        std::fwprintf(stderr,
                      L"GetData(FILEDESCRIPTORW) failed: hr=0x%08lx tymed=0x%lx\n",
                      static_cast<unsigned long>(g_hr),
                      static_cast<unsigned long>(sm_desc.tymed));
        obj->Release();
        result.failures = 1;
        return result;
    }

    auto* fg = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(sm_desc.hGlobal));
    if (fg == nullptr) {
        std::fwprintf(stderr, L"GlobalLock(FILEGROUPDESCRIPTORW) failed\n");
        ::ReleaseStgMedium(&sm_desc);
        obj->Release();
        result.failures = 1;
        return result;
    }
    result.files_seen = static_cast<int>(fg->cItems);
    std::fwprintf(stdout, L"FILEGROUPDESCRIPTORW: cItems=%u\n",
                  static_cast<unsigned>(fg->cItems));

    for (UINT i = 0; i < fg->cItems; ++i) {
        const FILEDESCRIPTORW& fd = fg->fgd[i];
        const std::uint64_t size =
            (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32)
            |  static_cast<std::uint64_t>(fd.nFileSizeLow);
        std::fwprintf(stdout,
                      L"  [%u] name=%ls size=%llu dwFlags=0x%lx attrs=0x%lx\n",
                      i, fd.cFileName,
                      static_cast<unsigned long long>(size),
                      static_cast<unsigned long>(fd.dwFlags),
                      static_cast<unsigned long>(fd.dwFileAttributes));

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::fwprintf(stdout, L"  (directory — skipping)\n");
            continue;
        }

        FORMATETC fe_cont{};
        fe_cont.cfFormat = static_cast<CLIPFORMAT>(cf_contents);
        fe_cont.dwAspect = DVASPECT_CONTENT;
        fe_cont.lindex   = static_cast<LONG>(i);
        fe_cont.tymed    = TYMED_ISTREAM;

        STGMEDIUM sm_cont{};
        const HRESULT chr = obj->GetData(&fe_cont, &sm_cont);
        if (FAILED(chr) || sm_cont.tymed != TYMED_ISTREAM || sm_cont.pstm == nullptr) {
            std::fwprintf(stderr,
                          L"  GetData(FILECONTENTS, lindex=%u, ISTREAM) failed: hr=0x%08lx tymed=0x%lx\n",
                          i,
                          static_cast<unsigned long>(chr),
                          static_cast<unsigned long>(sm_cont.tymed));
            ++result.failures;
            continue;
        }

        std::wstring full_path = dest_dir;
        if (!full_path.empty() &&
            full_path.back() != L'\\' && full_path.back() != L'/') {
            full_path += L'\\';
        }
        full_path += fd.cFileName;

        std::vector<std::uint8_t> captured;
        const bool ok = WriteStreamToFile(sm_cont.pstm, full_path, size,
                                          capture_content ? &captured : nullptr);
        if (ok) {
            ++result.files_written;
            if (capture_content) {
                result.contents.emplace_back(fd.cFileName, std::move(captured));
            }
        } else {
            ++result.failures;
        }
        ::ReleaseStgMedium(&sm_cont);
    }

    ::GlobalUnlock(sm_desc.hGlobal);
    ::ReleaseStgMedium(&sm_desc);
    obj->Release();
    return result;
}

// ─── Self-test mode ────────────────────────────────────────────────────────

int RunSelfTest(const std::wstring& base_dest) {
    std::fwprintf(stdout, L"=== Self-test mode ===\n");
    const std::wstring dest = MakeFreshSubdir(base_dest);
    std::fwprintf(stdout, L"destination: %ls\n", dest.c_str());

    // 1. Build the IDataObject with two test files.
    const auto files = SelfTestFiles();
    std::vector<VirtualFileSpec> specs;
    SelfTestChunkProvider provider;
    specs.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        VirtualFileSpec spec;
        spec.name         = files[i].name;
        spec.file_id      = std::to_string(i);
        spec.size         = files[i].size;
        spec.is_directory = false;
        provider.Add(spec.file_id, files[i].content, files[i].size);
        specs.push_back(std::move(spec));
    }

    IDataObject* obj = nullptr;
    HRESULT hr = CreateVirtualClipboardDataObject(std::move(specs),
                                                  "self-test-transfer",
                                                  &provider, &obj);
    if (FAILED(hr) || obj == nullptr) {
        std::fwprintf(stderr, L"CreateVirtualClipboardDataObject failed: hr=0x%08lx\n",
                      static_cast<unsigned long>(hr));
        return 10;
    }

    // 2. Publish to the OS clipboard.
    hr = ::OleSetClipboard(obj);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"OleSetClipboard failed: hr=0x%08lx\n",
                      static_cast<unsigned long>(hr));
        obj->Release();
        return 11;
    }
    std::fwprintf(stdout, L"OleSetClipboard: OK (%zu file(s) published).\n",
                  files.size());
    obj->Release();

    // 3. Read it back + write to disk.  Capture content so we can verify.
    const PasteResult pr = PasteFromClipboardToFolder(dest, /*capture_content=*/true);

    // 4. Clear clipboard so we don't leave a dangling IDataObject.
    ::OleSetClipboard(nullptr);
    ::OleFlushClipboard();

    // 5. Verify each captured payload matches the expected content.
    int mismatches = 0;
    for (std::size_t i = 0; i < pr.contents.size(); ++i) {
        const auto& got_name    = pr.contents[i].first;
        const auto& got_bytes   = pr.contents[i].second;
        // find the expected entry
        const SelfTestFile* expected = nullptr;
        for (const auto& f : files) {
            if (f.name == got_name) { expected = &f; break; }
        }
        if (expected == nullptr) {
            std::fwprintf(stderr, L"  UNEXPECTED file in output: %ls\n",
                          got_name.c_str());
            ++mismatches;
            continue;
        }
        if (got_bytes.size() != expected->size) {
            std::fwprintf(stderr,
                          L"  SIZE MISMATCH for %ls: got=%zu expected=%llu\n",
                          got_name.c_str(), got_bytes.size(),
                          static_cast<unsigned long long>(expected->size));
            ++mismatches;
            continue;
        }
        if (std::memcmp(got_bytes.data(), expected->content,
                        static_cast<std::size_t>(expected->size)) != 0) {
            std::fwprintf(stderr, L"  BYTE MISMATCH for %ls\n",
                          got_name.c_str());
            ++mismatches;
            continue;
        }
        std::fwprintf(stdout, L"  verified %ls (%zu bytes byte-identical)\n",
                      got_name.c_str(), got_bytes.size());
    }

    std::fwprintf(stdout,
                  L"=== Self-test result: files_seen=%d files_written=%d "
                  L"failures=%d mismatches=%d ===\n",
                  pr.files_seen, pr.files_written, pr.failures, mismatches);

    if (pr.files_written != static_cast<int>(files.size()) ||
        pr.failures != 0 || mismatches != 0) {
        return 1;
    }
    std::fwprintf(stdout, L"PASS — full publish→clipboard→read→disk-write chain works.\n");
    return 0;
}

}  // namespace

#include <map>  // included late to keep header order clean above

int wmain(int argc, wchar_t** argv) {
    ScopedOle ole;
    if (FAILED(ole.hr)) {
        std::fwprintf(stderr, L"OleInitialize failed: hr=0x%08lx\n",
                      static_cast<unsigned long>(ole.hr));
        return 1;
    }

    if (argc >= 2 && std::wcscmp(argv[1], L"--self-test") == 0) {
        const std::wstring base = (argc >= 3) ? argv[2] : TempDir();
        return RunSelfTest(base);
    }

    if (argc < 2) {
        std::fwprintf(stderr,
                      L"Usage:\n"
                      L"  %ls --self-test [<destination_folder>]\n"
                      L"      Self-contained publish+paste+verify (default dest = %%TEMP%%).\n"
                      L"  %ls <destination_folder>\n"
                      L"      Read current clipboard, paste virtual files to folder.\n",
                      argv[0], argv[0]);
        return 64;
    }

    const std::wstring dest = argv[1];
    const DWORD attrs = ::GetFileAttributesW(dest.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::fwprintf(stderr, L"destination is not a directory: %ls\n",
                      dest.c_str());
        return 2;
    }
    const PasteResult pr = PasteFromClipboardToFolder(dest, /*capture_content=*/false);
    std::fwprintf(stdout,
                  L"DONE: files_seen=%d files_written=%d failures=%d\n",
                  pr.files_seen, pr.files_written, pr.failures);
    return (pr.failures == 0 && pr.files_written > 0) ? 0 : 3;
}
