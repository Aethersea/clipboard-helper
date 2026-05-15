#include "virtual_file_provider.h"

#include <shlobj.h>     // CFSTR_FILEDESCRIPTORW, FILEGROUPDESCRIPTORW,
                        // CFSTR_PERFORMEDDROPEFFECT, FILE_ATTRIBUTE_*

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "clipboard_ops.h"   // GetCfFileDescriptor / GetCfFileContents
#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

// Defensive upper bound on the number of files we'll publish in a single
// CFSTR_FILEDESCRIPTORW announcement. Each FILEDESCRIPTORW is ~592 bytes,
// so the cap maps to ~60 MiB of HGLOBAL — large but bounded. A degenerate
// or malicious announcement that asks for millions of entries would
// otherwise drive GlobalAlloc into the ground and (more importantly)
// freeze the STA worker building the descriptor mid-paste.
constexpr std::size_t kMaxVirtualFileSpecs = 100'000;

// Build a FILEGROUPDESCRIPTORW HGLOBAL for `specs`. Returns nullptr on
// allocation failure; caller takes ownership of the HGLOBAL and is
// responsible for GlobalFree (or hands it to STGMEDIUM with
// pUnkForRelease=nullptr so ReleaseStgMedium does it for them).
HGLOBAL BuildFileGroupDescriptor(const std::vector<VirtualFileSpec>& specs) {
    if (specs.empty()) return nullptr;
    if (specs.size() > kMaxVirtualFileSpecs) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "BuildFileGroupDescriptor: rejecting %zu specs (cap=%zu) — "
                      "treat upstream as malformed",
                      specs.size(), kMaxVirtualFileSpecs);
        LH_LOG_WARN(buf);
        return nullptr;
    }
    // FILEGROUPDESCRIPTORW already includes one FILEDESCRIPTORW slot via
    // its `fgd[1]` flexible-ish trailing array. Allocate (N-1) extras
    // when N > 1; for N==1 the base size already covers it.
    const std::size_t base  = sizeof(FILEGROUPDESCRIPTORW);
    const std::size_t extra = (specs.size() - 1) * sizeof(FILEDESCRIPTORW);
    const std::size_t total = base + extra;

    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, total);
    if (h == nullptr) return nullptr;
    auto* fg = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(h));
    if (fg == nullptr) {
        ::GlobalFree(h);
        return nullptr;
    }
    std::memset(fg, 0, total);
    fg->cItems = static_cast<UINT>(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        FILEDESCRIPTORW& fd = fg->fgd[i];
        // FD_FILESIZE: nFileSizeLow/High are valid.
        // FD_ATTRIBUTES: dwFileAttributes is valid (dir vs file).
        // FD_PROGRESSUI: Shell may show its standard copy progress dialog
        //   while pulling bytes — this is the user-visible win over the
        //   old CF_HDROP path where Explorer just blocked silently.
        fd.dwFlags          = FD_FILESIZE | FD_ATTRIBUTES | FD_PROGRESSUI;
        fd.dwFileAttributes = specs[i].is_directory
            ? FILE_ATTRIBUTE_DIRECTORY
            : FILE_ATTRIBUTE_NORMAL;
        fd.nFileSizeLow  = static_cast<DWORD>(specs[i].size & 0xFFFFFFFFu);
        fd.nFileSizeHigh = static_cast<DWORD>(specs[i].size >> 32);
        // cFileName is MAX_PATH wchars (NUL-terminated). Truncate if the
        // incoming name is too long; the shell drop target will fail
        // gracefully on the truncated name rather than crash. Log so the
        // operator can correlate paste failures to MAX_PATH limits when
        // remote machines copy deeply-nested paths.
        const auto& n = specs[i].name;
        std::size_t copy = std::min<std::size_t>(n.size(), MAX_PATH - 1);
        // Truncating in the middle of a UTF-16 surrogate pair leaves a
        // dangling high surrogate at cFileName[copy-1], which the shell
        // renders as a replacement glyph and confuses some consumers'
        // path matching. Back off by one if we'd split a pair so we
        // truncate cleanly on a BMP/SMP boundary.
        if (copy < n.size() && copy > 0) {
            const wchar_t last = n[copy - 1];
            if (last >= 0xD800 && last <= 0xDBFF) {
                --copy;
            }
        }
        if (copy < n.size()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "BuildFileGroupDescriptor: truncating cFileName (%zu → %zu wchars)",
                          n.size(), copy);
            LH_LOG_WARN(buf);
        }
        // Copy with forward-slash → backslash normalization. macOS-side
        // FileMetadata.relative_path uses '/', and shell virtual-file
        // consumers (Outlook, 7-Zip, OneDrive Files-on-Demand) historically
        // only honor '\\' for the rebuilt subdirectory structure inside the
        // drop target. Modern Explorer handles '/' fine; legacy consumers
        // silently flatten the tree instead.
        for (std::size_t j = 0; j < copy; ++j) {
            const wchar_t c = n[j];
            fd.cFileName[j] = (c == L'/') ? L'\\' : c;
        }
        fd.cFileName[copy] = L'\0';
    }
    ::GlobalUnlock(h);
    return h;
}

// ─── VirtualFileEnum (IEnumFORMATETC) ────────────────────────────────────

class VirtualFileEnum final : public IEnumFORMATETC {
public:
    VirtualFileEnum(std::vector<FORMATETC> formats, ULONG pos)
        : formats_(std::move(formats)), pos_(pos) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IEnumFORMATETC)) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(::InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override {
        if (rgelt == nullptr) return E_POINTER;
        // pceltFetched is allowed to be NULL only when celt==1 (per IDL).
        if (pceltFetched == nullptr && celt != 1) return E_INVALIDARG;
        ULONG fetched = 0;
        while (fetched < celt && pos_ < static_cast<ULONG>(formats_.size())) {
            // For our static, NULL-ptd entries the consumer is expected
            // to leave ptd alone (no CoTaskMemAlloc'd structure to copy
            // through). Memcpy is safe.
            rgelt[fetched] = formats_[pos_];
            ++fetched;
            ++pos_;
        }
        if (pceltFetched != nullptr) *pceltFetched = fetched;
        return (fetched == celt) ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override {
        const ULONG remaining = static_cast<ULONG>(formats_.size()) - pos_;
        if (celt > remaining) {
            pos_ = static_cast<ULONG>(formats_.size());
            return S_FALSE;
        }
        pos_ += celt;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        pos_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** ppenum) override {
        if (ppenum == nullptr) return E_POINTER;
        auto* clone = new (std::nothrow) VirtualFileEnum(formats_, pos_);
        if (clone == nullptr) {
            *ppenum = nullptr;
            return E_OUTOFMEMORY;
        }
        *ppenum = clone;
        return S_OK;
    }

private:
    LONG                    refs_{1};
    std::vector<FORMATETC>  formats_;
    ULONG                   pos_{0};
};

// ─── VirtualFileStream (IStream) ─────────────────────────────────────────

class VirtualFileStream final : public IStream {
public:
    VirtualFileStream(std::string   transfer_id,
                      std::string   file_id,
                      std::wstring  display_name,
                      std::uint64_t size,
                      ChunkProvider* provider)
        : transfer_id_(std::move(transfer_id)),
          file_id_(std::move(file_id)),
          display_name_(std::move(display_name)),
          size_(size),
          provider_(provider) {}

    // ── IUnknown ────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown)
         || riid == __uuidof(ISequentialStream)
         || riid == __uuidof(IStream)) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(::InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    // ── ISequentialStream ───────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pv == nullptr) return STG_E_INVALIDPOINTER;
        if (pcbRead != nullptr) *pcbRead = 0;

        if (cb == 0) {
            return S_OK;
        }
        if (pos_ >= size_) {
            // Past EOF — well-defined: zero bytes read, S_OK. Some callers
            // also accept S_FALSE here; S_OK with *pcbRead=0 is the more
            // widely-supported convention (matches CFile / SHCreateStream).
            return S_OK;
        }
        // Clip the request to remaining file bytes so we never ask the
        // upstream provider for past-EOF chunks.
        const std::uint64_t remaining = size_ - pos_;
        const std::uint32_t want = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining,
                                    static_cast<std::uint64_t>(cb)));

        // Re-entrancy guard. FetchChunk may pump the STA message queue
        // while waiting on a pipe round-trip; a misbehaving consumer
        // that re-enters Read on the SAME stream during that window
        // would race pos_. Object lifetime is safe (the COM RPC stub
        // holds an in-call AddRef) but file-offset state would shred.
        // Reject re-entry with STG_E_INUSE.
        if (reading_) return STG_E_INUSE;
        reading_ = true;
        struct ReadingResetter { bool* flag; ~ReadingResetter() { *flag = false; } }
            resetter{&reading_};

        std::vector<std::uint8_t> chunk;
        bool is_last = false;
        const HRESULT hr = provider_->FetchChunk(transfer_id_, file_id_,
                                                  pos_, want, chunk, is_last);
        if (FAILED(hr)) {
            // Propagate as-is. STG_E_REVERTED, E_ABORT, E_FAIL all have
            // defined IStream::Read semantics on the caller side and the
            // shell copy engine respects them.
            return hr;
        }
        (void)is_last;  // advisory only — pcbRead conveys the same signal

        // Defensive clamp: a buggy ChunkProvider must never let us write
        // past the caller's buffer (`pv` is sized for `cb` bytes). `want`
        // is already ≤ cb, so capping at `want` doubles as the buffer cap.
        const ULONG got = static_cast<ULONG>(
            std::min<std::size_t>(chunk.size(), static_cast<std::size_t>(want)));
        if (got > 0) {
            std::memcpy(pv, chunk.data(), got);
            pos_ += got;
        }
        if (pcbRead != nullptr) *pcbRead = got;
        // S_OK regardless of short read — pcbRead conveys the count and
        // the caller re-issues Read for the next region. This matches the
        // behavior of file-system IStream wrappers and avoids the small
        // population of callers that treat S_FALSE as a hard EOF flag.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        (void)pv;
        (void)cb;
        if (pcbWritten != nullptr) *pcbWritten = 0;
        return STG_E_ACCESSDENIED;
    }

    // ── IStream ─────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove,
                                   DWORD dwOrigin,
                                   ULARGE_INTEGER* plibNewPosition) override {
        // All arithmetic in uint64 so a 2^63-byte file (purely theoretical
        // but undefined-cast-free) cannot wrap to a negative int64. We
        // bound the result against INT64_MAX so callers that try to seek
        // beyond what fits in signed 64-bit (which is the only thing the
        // STATSTG.cbSize signed view can represent) get STG_E_SEEKERROR
        // instead of a silently-wrapped position.
        std::uint64_t base = 0;
        switch (dwOrigin) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = pos_; break;
            case STREAM_SEEK_END: base = size_; break;
            default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t delta = dlibMove.QuadPart;
        std::uint64_t target_u = 0;
        if (delta >= 0) {
            const std::uint64_t d = static_cast<std::uint64_t>(delta);
            // base + d overflows iff d > UINT64_MAX - base.
            if (d > (std::numeric_limits<std::uint64_t>::max)() - base) {
                return STG_E_SEEKERROR;
            }
            target_u = base + d;
        } else {
            // Underflow: -delta would be UB at delta == LLONG_MIN, so
            // build the magnitude via unsigned arithmetic.
            const std::uint64_t mag = static_cast<std::uint64_t>(-(delta + 1)) + 1;
            if (mag > base) return STG_E_SEEKERROR;
            target_u = base - mag;
        }
        // Cap at INT64_MAX so STATSTG.cbSize semantics never overflow.
        if (target_u > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return STG_E_SEEKERROR;
        }
        // Allow seeking past EOF — IStream lets callers position the
        // cursor anywhere; the next Read returns 0 bytes per the EOF
        // branch above.
        pos_ = target_u;
        if (plibNewPosition != nullptr) {
            plibNewPosition->QuadPart = pos_;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override {
        (void)libNewSize;
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm,
                                     ULARGE_INTEGER cb,
                                     ULARGE_INTEGER* pcbRead,
                                     ULARGE_INTEGER* pcbWritten) override {
        if (pstm == nullptr) return STG_E_INVALIDPOINTER;
        if (pcbRead != nullptr)    pcbRead->QuadPart    = 0;
        if (pcbWritten != nullptr) pcbWritten->QuadPart = 0;
        // Fallback CopyTo: pull-then-push in a fixed-size buffer. The
        // shell copy engine rarely calls this (it Reads directly), but
        // implementing it cleanly satisfies the IStream contract.
        constexpr ULONG kChunk = 256 * 1024;
        std::vector<std::uint8_t> buf(kChunk);
        std::uint64_t total_read    = 0;
        std::uint64_t total_written = 0;
        std::uint64_t remaining     = cb.QuadPart;
        while (remaining > 0) {
            const ULONG take = static_cast<ULONG>(
                std::min<std::uint64_t>(remaining,
                                        static_cast<std::uint64_t>(kChunk)));
            ULONG got = 0;
            const HRESULT hr = Read(buf.data(), take, &got);
            if (FAILED(hr)) return hr;
            if (got == 0) break;  // EOF
            total_read += got;
            ULONG written = 0;
            const HRESULT hrw = pstm->Write(buf.data(), got, &written);
            if (FAILED(hrw)) return hrw;
            total_written += written;
            // Do NOT break on `got < take`: a short read can come from a
            // chunked network producer that hasn't reached EOF. Loop
            // naturally terminates when Read returns got==0.
            remaining -= got;
        }
        if (pcbRead != nullptr)    pcbRead->QuadPart    = total_read;
        if (pcbWritten != nullptr) pcbWritten->QuadPart = total_written;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) override {
        (void)grfCommitFlags;
        return S_OK;   // No-op for a read-only stream.
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset,
                                         ULARGE_INTEGER cb,
                                         DWORD dwLockType) override {
        (void)libOffset; (void)cb; (void)dwLockType;
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset,
                                           ULARGE_INTEGER cb,
                                           DWORD dwLockType) override {
        (void)libOffset; (void)cb; (void)dwLockType;
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (pstatstg == nullptr) return STG_E_INVALIDPOINTER;
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type             = STGTY_STREAM;
        pstatstg->cbSize.QuadPart  = size_;
        pstatstg->grfMode          = STGM_READ;
        if ((grfStatFlag & STATFLAG_NONAME) == 0) {
            const std::size_t name_bytes = (display_name_.size() + 1) * sizeof(wchar_t);
            pstatstg->pwcsName = static_cast<LPOLESTR>(::CoTaskMemAlloc(name_bytes));
            if (pstatstg->pwcsName == nullptr) {
                return STG_E_INSUFFICIENTMEMORY;
            }
            std::memcpy(pstatstg->pwcsName,
                        display_name_.data(),
                        display_name_.size() * sizeof(wchar_t));
            pstatstg->pwcsName[display_name_.size()] = L'\0';
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override {
        if (ppstm == nullptr) return E_POINTER;
        auto* clone = new (std::nothrow) VirtualFileStream(transfer_id_,
                                                            file_id_,
                                                            display_name_,
                                                            size_,
                                                            provider_);
        if (clone == nullptr) {
            *ppstm = nullptr;
            return E_OUTOFMEMORY;
        }
        clone->pos_ = pos_;
        *ppstm = clone;
        return S_OK;
    }

private:
    LONG            refs_{1};
    std::string     transfer_id_;
    std::string     file_id_;
    std::wstring    display_name_;
    std::uint64_t   size_;
    std::uint64_t   pos_{0};
    // Re-entrancy flag for Read. Protects pos_ against the same-thread,
    // same-stream re-entry pattern that arises when FetchChunk pumps the
    // STA message queue. Not a substitute for cross-stream synchroniza-
    // tion — distinct VirtualFileStream instances each have their own.
    bool            reading_{false};
    ChunkProvider*  provider_;  // borrowed, must outlive `this`
};

// ─── VirtualClipboardDataObject (IDataObject) ────────────────────────────

class VirtualClipboardDataObject final : public IDataObject {
public:
    VirtualClipboardDataObject(std::vector<VirtualFileSpec> specs,
                               std::string                  transfer_id,
                               ChunkProvider*               provider)
        : specs_(std::move(specs)),
          transfer_id_(std::move(transfer_id)),
          provider_(provider),
          cf_descriptor_(GetCfFileDescriptor()),
          cf_contents_(GetCfFileContents()) {}

    // ── IUnknown ────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(::InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    // ── IDataObject ─────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* sm) override {
        if (fe == nullptr || sm == nullptr) return DV_E_FORMATETC;
        std::memset(sm, 0, sizeof(*sm));

        const UINT fmt = fe->cfFormat;

        // CFSTR_FILEDESCRIPTORW → FILEGROUPDESCRIPTORW HGLOBAL.
        if (fmt == cf_descriptor_) {
            if ((fe->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
            HGLOBAL h = BuildFileGroupDescriptor(specs_);
            if (h == nullptr) return E_OUTOFMEMORY;
            sm->tymed          = TYMED_HGLOBAL;
            sm->hGlobal        = h;
            sm->pUnkForRelease = nullptr;  // OS frees via ReleaseStgMedium
            return S_OK;
        }

        // CFSTR_FILECONTENTS → IStream for the lindex-th file.
        if (fmt == cf_contents_) {
            if ((fe->tymed & TYMED_ISTREAM) == 0) return DV_E_TYMED;
            const LONG idx = fe->lindex;
            if (idx < 0 || static_cast<std::size_t>(idx) >= specs_.size()) {
                return DV_E_LINDEX;
            }
            const auto& spec = specs_[static_cast<std::size_t>(idx)];
            auto* stream = new (std::nothrow) VirtualFileStream(transfer_id_,
                                                                  spec.file_id,
                                                                  spec.name,
                                                                  spec.size,
                                                                  provider_);
            if (stream == nullptr) return E_OUTOFMEMORY;
            sm->tymed          = TYMED_ISTREAM;
            sm->pstm           = stream;     // refcount=1, ownership xferred
            sm->pUnkForRelease = nullptr;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* fe, STGMEDIUM* sm) override {
        (void)fe;
        (void)sm;
        // GetDataHere requires the caller to have pre-allocated the
        // medium. Shell virtual-file consumers do not use this entry
        // point — they call GetData. Returning E_NOTIMPL is the
        // documented "not supported" reply.
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (fe == nullptr) return DV_E_FORMATETC;
        const UINT fmt = fe->cfFormat;
        if (fmt == cf_descriptor_ && (fe->tymed & TYMED_HGLOBAL))  return S_OK;
        if (fmt == cf_contents_   && (fe->tymed & TYMED_ISTREAM))  return S_OK;
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in_fe,
                                                    FORMATETC* out_fe) override {
        (void)in_fe;
        if (out_fe == nullptr) return E_POINTER;
        // MSDN: when returning DATA_S_SAMEFORMATETC the contents of *out_fe
        // are undefined except that ptd must be set to NULL. Avoid the
        // zero-fill — some legacy consumers re-inspect cfFormat even on
        // the DATA_S_SAMEFORMATETC path; leaving an all-zero struct can
        // confuse them more than leaving prior garbage in place.
        out_fe->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* sm, BOOL release) override {
        (void)fe;
        (void)sm;
        (void)release;
        // We are a read-only producer. Some shell components call SetData
        // for CFSTR_PERFORMEDDROPEFFECT to inform the source about the
        // outcome of a drag-drop; not relevant to clipboard paste. Phase
        // 5 may revisit if a consumer relies on it.
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection,
                                            IEnumFORMATETC** ppEnum) override {
        if (ppEnum == nullptr) return E_POINTER;
        *ppEnum = nullptr;
        if (dwDirection != DATADIR_GET) return E_NOTIMPL;

        // Two FORMATETC entries:
        //   1. CFSTR_FILEDESCRIPTORW  / TYMED_HGLOBAL / lindex = -1
        //   2. CFSTR_FILECONTENTS     / TYMED_ISTREAM / lindex = -1
        //
        // For CFSTR_FILECONTENTS the enumeration advertises lindex=-1
        // (meaning "applies to any lindex"); consumers iterate lindex
        // themselves when calling GetData. This matches the convention
        // used by Outlook's IDataObject and the docs in CFSTR_FILECONTENTS.
        std::vector<FORMATETC> formats;
        formats.reserve(2);

        FORMATETC fe_desc{};
        fe_desc.cfFormat = static_cast<CLIPFORMAT>(cf_descriptor_);
        fe_desc.ptd      = nullptr;
        fe_desc.dwAspect = DVASPECT_CONTENT;
        fe_desc.lindex   = -1;
        fe_desc.tymed    = TYMED_HGLOBAL;
        formats.push_back(fe_desc);

        FORMATETC fe_cont{};
        fe_cont.cfFormat = static_cast<CLIPFORMAT>(cf_contents_);
        fe_cont.ptd      = nullptr;
        fe_cont.dwAspect = DVASPECT_CONTENT;
        fe_cont.lindex   = -1;
        fe_cont.tymed    = TYMED_ISTREAM;
        formats.push_back(fe_cont);

        auto* en = new (std::nothrow) VirtualFileEnum(std::move(formats), 0);
        if (en == nullptr) return E_OUTOFMEMORY;
        *ppEnum = en;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    LONG                          refs_{1};
    std::vector<VirtualFileSpec>  specs_;
    std::string                   transfer_id_;
    ChunkProvider*                provider_;  // borrowed, must outlive `this`
    UINT                          cf_descriptor_;
    UINT                          cf_contents_;
};

}  // namespace

HRESULT CreateVirtualClipboardDataObject(std::vector<VirtualFileSpec> specs,
                                         std::string                  transfer_id,
                                         ChunkProvider*               provider,
                                         IDataObject**                out_object) {
    if (out_object == nullptr) return E_POINTER;
    *out_object = nullptr;
    if (provider == nullptr) return E_INVALIDARG;
    if (specs.empty())       return E_INVALIDARG;

    auto* obj = new (std::nothrow) VirtualClipboardDataObject(std::move(specs),
                                                                std::move(transfer_id),
                                                                provider);
    if (obj == nullptr) return E_OUTOFMEMORY;
    *out_object = obj;
    return S_OK;
}

}  // namespace leviathan::clipboard_helper
