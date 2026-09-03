#pragma once
//
// DispatcherChunkProvider — the FILE_CHUNK_REQUEST / FILE_CHUNK_DATA
// round-trip behind every virtual-file paste.
//
// Pulled out of dispatch.cpp so the wait / correlation / cancellation
// rules can be unit-tested without a PipeServer, an OLE clipboard or the
// Dispatcher itself. The only two things the provider needs from the
// outside are "send this frame to the parent" (a callable, so tests can
// capture the request) and the per-chunk timeout (a constructor argument,
// so tests do not have to wait 30 s to observe a timeout).
//
// Threading:
//   * FetchChunk() runs on the STA worker thread (COM marshals the
//     IStream::Read call there). It sends a FILE_CHUNK_REQUEST and waits
//     for the parent's FILE_CHUNK_DATA reply via MsgWaitForMultipleObjects,
//     which keeps the STA pumping window messages so nested COM marshaling
//     can land.
//   * DeliverChunk() runs on the pipe-accept thread when the parent's
//     FILE_CHUNK_DATA arrives. It finds the matching PendingChunk by the
//     (transfer_id, file_id, offset) tuple, stages the bytes, and SetEvent's
//     that specific waiter.
//   * CancelAll() runs on whichever thread rotates clipboard ownership or
//     shuts the helper down. It wakes every in-flight waiter with
//     STG_E_REVERTED and installs no process-wide gate — later FetchChunk
//     calls (for a fresh ANNOUNCE_DELAYED) are served as normal.
//
// Concurrency model:
//   The STA's inner message pump inside FetchChunk WILL re-enter
//   IStream::Read for sibling streams (e.g. Explorer copying multiple
//   files in parallel — COM marshals the Read calls into the same STA,
//   and our pump dispatches them). The provider therefore must support
//   multiple in-flight requests at once. Each request gets its own auto-
//   reset event and its own PendingChunk struct; the map keyed by
//   (transfer_id, file_id, offset) is the only shared state.

#include <windows.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "virtual_file_provider.h"

namespace leviathan::clipboard_helper {

// Per-chunk timeout for the FILE_CHUNK_REQUEST round-trip a
// VirtualFileStream::Read does on the STA thread. Tighter than the legacy
// 120 s render budget because Explorer's copy dialog already shows progress
// — long stalls now surface as visible "slow" instead of a frozen shell.
// Caps at 30 s so a single WebRTC packet loss event doesn't pin the STA.
constexpr DWORD kDefaultChunkTimeoutMs = 30 * 1000;

class DispatcherChunkProvider final : public ChunkProvider {
public:
    // Sends one encoded HelperMessage frame to the parent. Returns false
    // when the transport is gone. In production this wraps
    // PipeServer::SendFrame.
    using SendFrameFn = std::function<bool(const std::vector<std::uint8_t>&)>;

    // An empty `send_frame` means "no transport": every FetchChunk fails
    // with E_FAIL before staging anything.
    explicit DispatcherChunkProvider(SendFrameFn send_frame,
                                     DWORD       timeout_ms = kDefaultChunkTimeoutMs);
    ~DispatcherChunkProvider() override;

    DispatcherChunkProvider(const DispatcherChunkProvider&)            = delete;
    DispatcherChunkProvider& operator=(const DispatcherChunkProvider&) = delete;

    // ChunkProvider. Return values (see virtual_file_provider.h):
    //   S_OK            — reply received without error; bytes in out_data
    //   E_INVALIDARG    — empty transfer_id or file_id
    //   E_FAIL          — no transport, send failed, timed out, or the
    //                     parent replied with an error string
    //   STG_E_REVERTED  — CancelAll() ran while this request was in flight
    //   E_ABORT         — WM_QUIT arrived while pumping (re-posted so the
    //                     STA's outer loop still sees it)
    HRESULT FetchChunk(const std::string&         transfer_id,
                       const std::string&         file_id,
                       std::uint64_t              offset,
                       std::uint32_t              size,
                       std::vector<std::uint8_t>& out_data,
                       bool&                      out_is_last) override;

    // Pipe thread → STA thread bridge. Pops the OLDEST pending request for
    // the (transfer_id, file_id, offset) tuple, stages bytes (or the
    // parent's error string), and signals that request's event — all under
    // the lock, so one reply consumes exactly one request and two replies
    // for the same tuple complete two waiters in FIFO order. A tuple with
    // no waiter (timed out / cancelled / unknown) is dropped quietly —
    // that's how requests cancelled by a superseded clipboard cycle drain.
    // PendingCount() drops when the reply lands, not when the waiter wakes.
    void DeliverChunk(const std::string&         transfer_id,
                      const std::string&         file_id,
                      std::uint64_t              offset,
                      std::vector<std::uint8_t>  data,
                      bool                       is_last,
                      const std::string&         error);

    // Wake every IN-FLIGHT FetchChunk so the helper's shutdown /
    // clipboard-ownership-rotation path doesn't hang the STA. Each
    // existing slot is flagged cancelled and its event is set so the
    // FetchChunk wait wakes, sees slot->cancelled, and returns
    // STG_E_REVERTED. Requests issued after this call are unaffected.
    void CancelAll();

    // Number of FetchChunk calls whose reply has not landed yet (a
    // delivered request no longer counts even if its waiter has not woken).
    // Exposed for tests and diagnostics.
    std::size_t PendingCount() const;

    DWORD timeout_ms() const { return timeout_ms_; }

private:
    struct PendingChunk {
        HANDLE                       event{nullptr};
        std::vector<std::uint8_t>    data;
        bool                         is_last{false};
        std::string                  error;
        bool                         received{false};
        // Set by CancelAll on existing slots when clipboard ownership
        // rotates or the helper shuts down. Per-slot rather than process-
        // wide so the FetchChunk path can stay open for fresh
        // announcements after a rotation.
        bool                         cancelled{false};
    };

    // Composite key for the in-flight FetchChunk map. The wire fields the
    // parent echoes back (transfer_id, file_id, offset) uniquely identify
    // a request within the current process — file_id is per-announcement
    // and offset prevents collisions when the shell issues sequential
    // Reads on the same stream before the previous one has returned (rare
    // but legal). Format is "<transfer_id>|<file_id>|<decimal-offset>";
    // both transfer_id and file_id are opaque tokens (no embedded '|' is
    // expected, but if one slips through the worst case is a missed
    // correlation that triggers a FetchChunk timeout, not data corruption).
    static std::string MakeKey(const std::string& transfer_id,
                               const std::string& file_id,
                               std::uint64_t      offset);

    // Remove a specific slot from its per-key queue. Caller MUST hold mu_.
    // If the queue becomes empty the key is erased so the map doesn't
    // accumulate orphan empty queues across long-running sessions.
    void EraseFromQueueLocked(const std::string&                   key,
                              const std::shared_ptr<PendingChunk>& slot);

    SendFrameFn        send_frame_;
    DWORD              timeout_ms_;
    mutable std::mutex mu_;
    // Per-key FIFO queue of in-flight FetchChunk requests. A queue rather
    // than a single slot so two consumers reading the SAME
    // (transfer_id, file_id, offset) tuple don't strand each other (see
    // FetchChunk staging block). DeliverChunk pops the front on delivery;
    // a waiter that gives up (timeout / cancel / WM_QUIT) removes its own
    // still-queued slot via EraseFromQueueLocked. Event HANDLEs are
    // signalled and closed only under mu_.
    std::unordered_map<std::string, std::deque<std::shared_ptr<PendingChunk>>> pending_;
};

}  // namespace leviathan::clipboard_helper
