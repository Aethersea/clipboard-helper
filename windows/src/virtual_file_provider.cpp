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

// Build a FILEGROUPDESCRIPTORW HGLOBAL for `specs`. Returns nullptr on
// allocation failure; caller takes ownership of the HGLOBAL and is
// responsible for GlobalFree (or hands it to STGMEDIUM with
// pUnkForRelease=nullptr so ReleaseStgMedium does it for them).
//
// The byte layout itself comes from BuildFileGroupDescriptorPayload in
// clipboard_format.cpp so the descriptor packing can be unit-tested
// without OLE / STGMEDIUM in the test binary.
HGLOBAL BuildFileGroupDescriptor(const std::vector<VirtualFileSpec>& specs) {
    if (specs.size() > kMaxVirtualFileSpecs) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "BuildFileGroupDescriptor: rejecting %zu specs (cap=%zu) — "
                      "treat upstream as malformed",
                      specs.size(), kMaxVirtualFileSpecs);
        LH_LOG_WARN(buf);
        return nullptr;
    }
    // Dump descriptor contents — Explorer rejects file pastes silently when
    // file_size == 0 with FD_FILESIZE flag, or when cFileName has illegal
    // chars (':' / '*' / '?' / '<' / '>' / '|' / '"' / '\') on Windows
    // filesystems. Trace lets the operator diff the published descriptor
    // against Explorer's drop-target validation rules.
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "[trace] BuildFileGroupDescriptor: count=%zu", specs.size());
        LH_LOG_INFO(buf);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& s = specs[i];
            // Best-effort log of the UTF-16 name as ANSI — non-ASCII chars
            // show as '?' but that's enough to spot path-separator issues.
            std::string ansi;
            ansi.reserve(s.name.size());
            for (wchar_t c : s.name) {
                ansi.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
            }
            std::snprintf(buf, sizeof(buf),
                          "[trace]   spec[%zu] file_id=%s size=%llu is_dir=%d name=\"%s\"",
                          i, s.file_id.c_str(),
                          static_cast<unsigned long long>(s.size),
                          s.is_directory ? 1 : 0,
                          ansi.c_str());
            LH_LOG_INFO(buf);
        }
    }
    // Pre-scan for cFileName truncations so the operator can correlate
    // paste failures on deeply-nested paths to MAX_PATH limits. The pure
    // BuildFileGroupDescriptorPayload helper is silent by design; we
    // observe at the wrapper boundary instead.
    for (const auto& s : specs) {
        if (s.name.size() > MAX_PATH - 1) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "BuildFileGroupDescriptor: truncating cFileName (%zu → ≤%zu wchars)",
                          s.name.size(),
                          static_cast<std::size_t>(MAX_PATH - 1));
            LH_LOG_WARN(buf);
        }
    }
    const std::vector<std::uint8_t> payload = BuildFileGroupDescriptorPayload(specs);
    if (payload.empty()) return nullptr;

    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, payload.size());
    if (h == nullptr) return nullptr;
    auto* base = static_cast<std::uint8_t*>(::GlobalLock(h));
    if (base == nullptr) {
        ::GlobalFree(h);
        return nullptr;
    }
    std::memcpy(base, payload.data(), payload.size());
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
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualFileEnum::Next celt=%lu fetched=%lu total=%zu pos=%lu",
                          static_cast<unsigned long>(celt),
                          static_cast<unsigned long>(fetched),
                          formats_.size(),
                          static_cast<unsigned long>(pos_));
            LH_LOG_INFO(buf);
        }
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
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualFileStream::Read file_id=%s cb=%lu pos=%llu size=%llu",
                          file_id_.c_str(),
                          static_cast<unsigned long>(cb),
                          static_cast<unsigned long long>(pos_),
                          static_cast<unsigned long long>(size_));
            LH_LOG_INFO(buf);
        }
        if (pv == nullptr) return STG_E_INVALIDPOINTER;
        if (pcbRead != nullptr) *pcbRead = 0;

        if (cb == 0) {
            return S_OK;
        }
        if (pos_ >= size_) {
            // FreeRDP convention: short/EOF read returns S_FALSE so the
            // shell copy engine can distinguish "no more bytes" from
            // "successful full read." This matches rdpclip.exe's IStream
            // implementation which works reliably on Win11 paste paths
            // where S_OK + pcbRead=0 sometimes confuses the consumer.
            return S_FALSE;
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
            // Provider failed (timeout / peer error / cancellation).  Log so
            // the operator can correlate Explorer's paste-abort dialog with
            // the upstream cause; the helper would otherwise return silently
            // and Explorer's UI just says "couldn't be copied" with no detail.
            //
            // STG_E_REVERTED, E_ABORT, E_FAIL all have defined IStream::Read
            // semantics on the consumer side and the shell copy engine
            // respects them — propagating the upstream hr as-is is the right
            // call.
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "VirtualFileStream::Read: FetchChunk failed hr=0x%08lx "
                          "(file_id=%s offset=%llu want=%u) — propagating to caller",
                          static_cast<unsigned long>(hr),
                          file_id_.c_str(),
                          static_cast<unsigned long long>(pos_),
                          want);
            LH_LOG_WARN(buf);
            return hr;
        }

        // Defensive clamp: a buggy ChunkProvider must never let us write
        // past the caller's buffer (`pv` is sized for `cb` bytes). `want`
        // is already ≤ cb, so capping at `want` doubles as the buffer cap.
        const ULONG got = static_cast<ULONG>(
            std::min<std::size_t>(chunk.size(), static_cast<std::size_t>(want)));

        // Silent-truncation defense.  If the provider returned success but
        // an empty chunk while there were supposed to be bytes left
        // (`pos_ < size_` AND `want > 0`), the resulting `*pcbRead = 0` +
        // `return S_FALSE` would be indistinguishable from EOF to the shell
        // copy engine — Explorer would write a truncated file silently.
        // Causes we've actually seen / can imagine:
        //   * announcement size disagrees with the real source file size
        //     (shen-side metadata stale by the time paste fires).
        //   * source peer's chunk forwarder responded with empty data and
        //     no error string (would be a peer bug; treat defensively).
        // Failing the read loudly is strictly better than producing a
        // wrong-content file: the operator sees an error dialog instead of
        // discovering corruption later.
        if (got == 0 && want > 0) {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "VirtualFileStream::Read: provider returned empty chunk "
                          "while %u bytes were still expected (file_id=%s pos=%llu size=%llu is_last=%d) — "
                          "returning E_FAIL to avoid silent truncation",
                          want, file_id_.c_str(),
                          static_cast<unsigned long long>(pos_),
                          static_cast<unsigned long long>(size_),
                          is_last ? 1 : 0);
            LH_LOG_WARN(buf);
            return E_FAIL;
        }
        (void)is_last;  // advisory only — pcbRead conveys the same signal

        if (got > 0) {
            std::memcpy(pv, chunk.data(), got);
            pos_ += got;
        }
        if (pcbRead != nullptr) *pcbRead = got;
        // EOF detection by stream POSITION, not chunk length.  Returning
        // S_FALSE the moment got < cb (a "short fill") would silently
        // truncate the file when the upstream provider returned fewer
        // bytes than `want` mid-stream (shen-side bug, partial network
        // delivery, etc.) — Explorer treats S_FALSE as "no more bytes,
        // stop reading" and commits the truncated file.
        //
        // The right invariant: only return S_FALSE when we actually
        // reached the declared end of file (pos_ == size_).  If we got a
        // short fill mid-stream, return S_OK; Explorer will issue another
        // Read at the new pos_ and either get the rest, hit the
        // empty-chunk defense above (→ E_FAIL on a true zero-byte
        // response), or naturally reach EOF.
        return (pos_ >= size_) ? S_FALSE : S_OK;
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
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualFileStream::Seek file_id=%s move=%lld origin=%lu pos=%llu",
                          file_id_.c_str(),
                          static_cast<long long>(dlibMove.QuadPart),
                          static_cast<unsigned long>(dwOrigin),
                          static_cast<unsigned long long>(pos_));
            LH_LOG_INFO(buf);
        }
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
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualFileStream::Stat file_id=%s size=%llu flag=0x%lx",
                          file_id_.c_str(),
                          static_cast<unsigned long long>(size_),
                          static_cast<unsigned long>(grfStatFlag));
            LH_LOG_INFO(buf);
        }
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
    // IDataObjectAsyncCapability was multi-inherited briefly on 2026-05-20
    // following agy's recommendation, but removed the same day because:
    //   1. The OLE clipboard wrapper around our IDataObject only marshals
    //      IDataObject — Explorer's QI for IDataObjectAsyncCapability
    //      hits the proxy and never reaches our class (confirmed: our
    //      QueryInterface trace never logged the async-cap IID even once).
    //   2. Multi-inheriting two COM interfaces on the same class needed
    //      manual disambiguation that the proxy's IUnknown probe could
    //      have misinterpreted.
    // Cross-process clipboard pastes don't need IDataObjectAsyncCapability
    // anyway — the shell's clipboard paste path already runs GetData on
    // a worker thread.  (drag-drop is the case where it's needed.)
public:
    VirtualClipboardDataObject(std::vector<VirtualFileSpec> specs,
                               std::string                  transfer_id,
                               ChunkProvider*               provider)
        : specs_(std::move(specs)),
          transfer_id_(std::move(transfer_id)),
          provider_(provider),
          cf_descriptor_(GetCfFileDescriptor()),
          cf_contents_(GetCfFileContents()),
          cf_drop_effect_(GetCfPreferredDropEffect()),
          cf_file_attrs_(GetCfFileAttributesArray()) {}
    // 2026-05-20: stripped Exclude-from-monitor and per-file lindex
    // experiments back to a near-FreeRDP advertise list.  Kept
    // CFSTR_PREFERREDDROPEFFECT because removing it makes Explorer 11
    // not enable the Paste menu at all (different from the silent-abort
    // failure mode — when paste is even available, the drop-effect entry
    // is needed for Explorer to categorize us as a file source).

    // ── IUnknown ────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            LH_LOG_INFO("[trace] VirtualClipboardDataObject::QueryInterface(IDataObject) → S_OK");
            return S_OK;
        }
        *ppv = nullptr;
        {
            // Log unknown IID requests so we can see when consumers probe
            // for other interfaces (IStorageProviderUriSource, etc.).
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualClipboardDataObject::QueryInterface unknown IID "
                          "{%08lX-%04hX-%04hX-...} → E_NOINTERFACE",
                          static_cast<unsigned long>(riid.Data1),
                          static_cast<unsigned short>(riid.Data2),
                          static_cast<unsigned short>(riid.Data3));
            LH_LOG_INFO(buf);
        }
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
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualClipboardDataObject::GetData "
                          "cfFormat=%u (descriptor=%u contents=%u) lindex=%ld tymed=0x%lx",
                          fmt, cf_descriptor_, cf_contents_,
                          static_cast<long>(fe->lindex),
                          static_cast<unsigned long>(fe->tymed));
            LH_LOG_INFO(buf);
        }

        // CFSTR_FILEDESCRIPTORW → FILEGROUPDESCRIPTORW HGLOBAL.
        if (fmt == cf_descriptor_) {
            if ((fe->tymed & TYMED_HGLOBAL) == 0) {
                LH_LOG_INFO("[trace] GetData: DESCRIPTOR but tymed missing TYMED_HGLOBAL → DV_E_TYMED");
                return DV_E_TYMED;
            }
            HGLOBAL h = BuildFileGroupDescriptor(specs_);
            if (h == nullptr) return E_OUTOFMEMORY;
            sm->tymed          = TYMED_HGLOBAL;
            sm->hGlobal        = h;
            sm->pUnkForRelease = nullptr;  // OS frees via ReleaseStgMedium
            LH_LOG_INFO("[trace] GetData: returning FILEGROUPDESCRIPTORW");
            return S_OK;
        }

        // "File Attributes Array" → cItems + OR + AND + per-item attrs.
        // Built from specs_[i].is_directory; mirrors what we already pack
        // into FILEGROUPDESCRIPTORW.dwFileAttributes.
        if (fmt == cf_file_attrs_) {
            if ((fe->tymed & TYMED_HGLOBAL) == 0) {
                LH_LOG_INFO("[trace] GetData: FILE_ATTRIBUTES_ARRAY but tymed missing TYMED_HGLOBAL → DV_E_TYMED");
                return DV_E_TYMED;
            }
            const std::size_t n = specs_.size();
            // Layout: UINT cItems; DWORD or; DWORD and; DWORD rg[n]; (cItems may be 0
            // but our IDataObject rejects empty specs at construction so n >= 1.)
            const std::size_t total = sizeof(UINT) + 2 * sizeof(DWORD)
                                    + n * sizeof(DWORD);
            HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, total);
            if (h == nullptr) return E_OUTOFMEMORY;
            auto* base = static_cast<std::uint8_t*>(::GlobalLock(h));
            if (base == nullptr) {
                ::GlobalFree(h);
                return E_OUTOFMEMORY;
            }
            DWORD or_attrs  = 0;
            DWORD and_attrs = 0xFFFFFFFFu;
            for (std::size_t i = 0; i < n; ++i) {
                const DWORD a = specs_[i].is_directory
                    ? FILE_ATTRIBUTE_DIRECTORY
                    : FILE_ATTRIBUTE_NORMAL;
                or_attrs  |= a;
                and_attrs &= a;
            }
            std::uint8_t* p = base;
            const UINT count = static_cast<UINT>(n);
            std::memcpy(p, &count, sizeof(count));           p += sizeof(count);
            std::memcpy(p, &or_attrs, sizeof(or_attrs));     p += sizeof(or_attrs);
            std::memcpy(p, &and_attrs, sizeof(and_attrs));   p += sizeof(and_attrs);
            for (std::size_t i = 0; i < n; ++i) {
                const DWORD a = specs_[i].is_directory
                    ? FILE_ATTRIBUTE_DIRECTORY
                    : FILE_ATTRIBUTE_NORMAL;
                std::memcpy(p, &a, sizeof(a));               p += sizeof(a);
            }
            ::GlobalUnlock(h);
            sm->tymed          = TYMED_HGLOBAL;
            sm->hGlobal        = h;
            sm->pUnkForRelease = nullptr;
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[trace] GetData: returning FILE_ATTRIBUTES_ARRAY (cItems=%u or=0x%lx and=0x%lx)",
                              count,
                              static_cast<unsigned long>(or_attrs),
                              static_cast<unsigned long>(and_attrs));
                LH_LOG_INFO(buf);
            }
            return S_OK;
        }

        // CFSTR_PREFERREDDROPEFFECT → DWORD DROPEFFECT_COPY.
        if (fmt == cf_drop_effect_) {
            if ((fe->tymed & TYMED_HGLOBAL) == 0) {
                LH_LOG_INFO("[trace] GetData: PREFERREDDROPEFFECT but tymed missing TYMED_HGLOBAL → DV_E_TYMED");
                return DV_E_TYMED;
            }
            HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (h == nullptr) return E_OUTOFMEMORY;
            auto* p = static_cast<DWORD*>(::GlobalLock(h));
            if (p == nullptr) {
                ::GlobalFree(h);
                return E_OUTOFMEMORY;
            }
            *p = DROPEFFECT_COPY;
            ::GlobalUnlock(h);
            sm->tymed          = TYMED_HGLOBAL;
            sm->hGlobal        = h;
            sm->pUnkForRelease = nullptr;
            LH_LOG_INFO("[trace] GetData: returning DROPEFFECT_COPY");
            return S_OK;
        }

        // CFSTR_FILECONTENTS → IStream for the lindex-th file.
        if (fmt == cf_contents_) {
            if ((fe->tymed & TYMED_ISTREAM) == 0) {
                LH_LOG_INFO("[trace] GetData: CONTENTS but tymed missing TYMED_ISTREAM → DV_E_TYMED");
                return DV_E_TYMED;
            }
            const LONG idx = fe->lindex;
            if (idx < 0 || static_cast<std::size_t>(idx) >= specs_.size()) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "[trace] GetData: CONTENTS lindex=%ld out of range (specs=%zu) → DV_E_LINDEX",
                              static_cast<long>(idx), specs_.size());
                LH_LOG_INFO(buf);
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
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "[trace] GetData: returning IStream for lindex=%ld file_id=%s",
                              static_cast<long>(idx), spec.file_id.c_str());
                LH_LOG_INFO(buf);
            }
            return S_OK;
        }

        {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "[trace] GetData: cfFormat=%u not in [desc=%u, contents=%u, drop=%u] → DV_E_FORMATETC",
                          fmt, cf_descriptor_, cf_contents_, cf_drop_effect_);
            LH_LOG_INFO(buf);
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* fe, STGMEDIUM* sm) override {
        (void)sm;
        {
            char buf[160];
            const UINT fmt = fe ? fe->cfFormat : 0;
            const long lindex = fe ? static_cast<long>(fe->lindex) : 0;
            const unsigned long tymed = fe ? static_cast<unsigned long>(fe->tymed) : 0;
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualClipboardDataObject::GetDataHere "
                          "cfFormat=%u lindex=%ld tymed=0x%lx → E_NOTIMPL",
                          fmt, lindex, tymed);
            LH_LOG_INFO(buf);
        }
        // GetDataHere requires the caller to have pre-allocated the
        // medium. Shell virtual-file consumers do not use this entry
        // point — they call GetData. Returning E_NOTIMPL is the
        // documented "not supported" reply.
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (fe == nullptr) return DV_E_FORMATETC;
        const UINT fmt = fe->cfFormat;
        HRESULT hr;
        if      (fmt == cf_descriptor_  && (fe->tymed & TYMED_HGLOBAL)) hr = S_OK;
        else if (fmt == cf_contents_    && (fe->tymed & TYMED_ISTREAM)) hr = S_OK;
        else if (fmt == cf_drop_effect_ && (fe->tymed & TYMED_HGLOBAL)) hr = S_OK;
        else                                                             hr = DV_E_FORMATETC;
        {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualClipboardDataObject::QueryGetData "
                          "cfFormat=%u (descriptor=%u contents=%u) lindex=%ld tymed=0x%lx → hr=0x%08lx",
                          fmt, cf_descriptor_, cf_contents_,
                          static_cast<long>(fe->lindex),
                          static_cast<unsigned long>(fe->tymed),
                          static_cast<unsigned long>(hr));
            LH_LOG_INFO(buf);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in_fe,
                                                    FORMATETC* out_fe) override {
        {
            const UINT fmt = in_fe ? in_fe->cfFormat : 0;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "[trace] GetCanonicalFormatEtc in_cfFormat=%u → DATA_S_SAMEFORMATETC",
                          fmt);
            LH_LOG_INFO(buf);
        }
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
        // Accept shell post-paste notification formats and silently
        // discard them.  The OLE clipboard proxy probes SetData with
        // these during OleSetClipboard to confirm the source honors the
        // paste-state contract; returning E_NOTIMPL here observably
        // makes the proxy decline to forward subsequent GetData calls
        // (paste_roundtrip_test went from 4/4 → flaky).
        //   CFSTR_PERFORMEDDROPEFFECT       — DWORD: which DROPEFFECT_*
        //   CFSTR_PASTESUCCEEDED            — DWORD: 0/1
        //   CFSTR_LOGICALPERFORMEDDROPEFFECT — DWORD: logical effect
        const UINT fmt = fe ? fe->cfFormat : 0;
        const UINT cf_performed = ::RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
        const UINT cf_paste_ok  = ::RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED);
        const UINT cf_logical   = ::RegisterClipboardFormatW(CFSTR_LOGICALPERFORMEDDROPEFFECT);
        if (fmt == cf_performed || fmt == cf_paste_ok || fmt == cf_logical) {
            if (release && sm) ::ReleaseStgMedium(sm);
            return S_OK;
        }
        (void)sm; (void)release;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection,
                                            IEnumFORMATETC** ppEnum) override {
        if (ppEnum == nullptr) return E_POINTER;
        *ppEnum = nullptr;
        if (dwDirection != DATADIR_GET) return E_NOTIMPL;

        // Advertise list (kept lean — no Exclude-from-monitor; that one
        // was tried and didn't change the failure mode):
        //
        //   1. CFSTR_PREFERREDDROPEFFECT / TYMED_HGLOBAL / lindex = -1
        //      DWORD = DROPEFFECT_COPY.  Without this Explorer 11 doesn't
        //      enable the Paste menu against our IDataObject at all.
        //   2. CFSTR_FILEDESCRIPTORW    / TYMED_HGLOBAL / lindex = -1
        //      Single descriptor blob covering all files.
        //   3. CFSTR_FILECONTENTS       / TYMED_ISTREAM / lindex = -1
        //      Probe entry — OLE's intermediate clipboard object uses
        //      this to QueryGetData "do you have CONTENTS at all" before
        //      iterating per-file requests.
        //   4. CFSTR_FILECONTENTS       / TYMED_ISTREAM / lindex = i
        //      One entry per file in `specs_`.  Shell calls
        //      GetData(contents, lindex=i, ISTREAM) for each.
        std::vector<FORMATETC> formats;
        formats.reserve(3 + specs_.size());

        FORMATETC fe_drop{};
        fe_drop.cfFormat = static_cast<CLIPFORMAT>(cf_drop_effect_);
        fe_drop.ptd      = nullptr;
        fe_drop.dwAspect = DVASPECT_CONTENT;
        fe_drop.lindex   = -1;
        fe_drop.tymed    = TYMED_HGLOBAL;
        formats.push_back(fe_drop);

        FORMATETC fe_desc{};
        fe_desc.cfFormat = static_cast<CLIPFORMAT>(cf_descriptor_);
        fe_desc.ptd      = nullptr;
        fe_desc.dwAspect = DVASPECT_CONTENT;
        fe_desc.lindex   = -1;
        fe_desc.tymed    = TYMED_HGLOBAL;
        formats.push_back(fe_desc);

        FORMATETC fe_cont_probe{};
        fe_cont_probe.cfFormat = static_cast<CLIPFORMAT>(cf_contents_);
        fe_cont_probe.ptd      = nullptr;
        fe_cont_probe.dwAspect = DVASPECT_CONTENT;
        fe_cont_probe.lindex   = -1;
        fe_cont_probe.tymed    = TYMED_ISTREAM;
        formats.push_back(fe_cont_probe);

        for (std::size_t i = 0; i < specs_.size(); ++i) {
            FORMATETC fe_cont{};
            fe_cont.cfFormat = static_cast<CLIPFORMAT>(cf_contents_);
            fe_cont.ptd      = nullptr;
            fe_cont.dwAspect = DVASPECT_CONTENT;
            fe_cont.lindex   = static_cast<LONG>(i);
            fe_cont.tymed    = TYMED_ISTREAM;
            formats.push_back(fe_cont);
        }

        const std::size_t format_count = formats.size();
        auto* en = new (std::nothrow) VirtualFileEnum(std::move(formats), 0);
        if (en == nullptr) return E_OUTOFMEMORY;
        *ppEnum = en;
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[trace] VirtualClipboardDataObject::EnumFormatEtc returned %zu formats "
                          "(drop-effect + descriptor + contents(-1) + %zu per-file contents)",
                          format_count, specs_.size());
            LH_LOG_INFO(buf);
        }
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
    // CFSTR_PREFERREDDROPEFFECT — DWORD = DROPEFFECT_COPY (1). Required
    // for Explorer 11 to enable the Paste menu against our IDataObject;
    // without it the paste UI doesn't even appear.
    UINT                          cf_drop_effect_;
    // "File Attributes Array" — see Raymond Chen
    // https://devblogs.microsoft.com/oldnewthing/20140609-00/?p=783.
    // Lets the shell skip disk-attribute lookups during paste validation.
    UINT                          cf_file_attrs_;
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
