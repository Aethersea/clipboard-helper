#include "chunk_provider.h"

#include <cstdio>
#include <utility>

#include "dispatch_codec.h"
#include "log.h"

namespace leviathan::clipboard_helper {

DispatcherChunkProvider::DispatcherChunkProvider(SendFrameFn send_frame, DWORD timeout_ms)
    : send_frame_(std::move(send_frame)), timeout_ms_(timeout_ms) {}

DispatcherChunkProvider::~DispatcherChunkProvider() {
    // Close any leftover events. In practice ReleaseStaBoundResources
    // → CancelAll has already cleaned out the map by the time we get
    // here; this is defensive against torn shutdown paths.
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [_, queue] : pending_) {
        for (auto& slot : queue) {
            if (slot && slot->event != nullptr) {
                ::CloseHandle(slot->event);
                slot->event = nullptr;
            }
        }
    }
    pending_.clear();
}

HRESULT DispatcherChunkProvider::FetchChunk(const std::string&         transfer_id,
                                            const std::string&         file_id,
                                            std::uint64_t              offset,
                                            std::uint32_t              size,
                                            std::vector<std::uint8_t>& out_data,
                                            bool&                      out_is_last) {
    out_data.clear();
    out_is_last = false;
    if (!send_frame_) return E_FAIL;
    if (transfer_id.empty() || file_id.empty()) return E_INVALIDARG;

    // Correlate replies by the (transfer_id, file_id, offset) tuple
    // the parent already echoes back in HelperFileChunkData. transfer_id
    // is the announcement-scoped identifier shen uses to route the
    // request upstream to leviathan; file_id+offset disambiguate
    // requests within an announcement.
    const std::string key = MakeKey(transfer_id, file_id, offset);

    auto slot = std::make_shared<PendingChunk>();
    slot->event = ::CreateEventW(nullptr, /*manualReset=*/FALSE,
                                 /*initial=*/FALSE, nullptr);
    if (slot->event == nullptr) {
        LH_LOG_ERROR("DispatcherChunkProvider: CreateEventW failed");
        return E_FAIL;
    }
    // Stage in the per-key FIFO queue BEFORE sending the frame so the
    // pipe thread can find us as soon as the reply lands. A FIFO (not
    // a single slot) lets us survive the edge case where two distinct
    // streams happen to issue overlapping Reads at the SAME
    // (transfer_id, file_id, offset) — without a queue the second
    // staging would overwrite the first's slot and strand the first
    // request until timeout. In practice the shell never does this,
    // but a queue is cheap and removes a class of timeout. No
    // process-wide "canceled" gate either — cancellation is per-slot
    // via PendingChunk::cancelled set by CancelAll on existing waiters.
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_[key].push_back(slot);
    }

    const auto frame = dispatch_codec::EncodeFileChunkRequestOutbound(
        transfer_id, file_id, offset, size);
    if (!send_frame_(frame)) {
        std::lock_guard<std::mutex> lock(mu_);
        EraseFromQueueLocked(key, slot);
        ::CloseHandle(slot->event);
        slot->event = nullptr;
        return E_FAIL;
    }

    // Wait on this request's specific event. MsgWaitForMultipleObjects
    // with QS_ALLINPUT keeps the STA pumping so sibling IStream::Read
    // calls (for other files in a multi-file paste) and the helper's
    // own WM_CLIPBOARDUPDATE notifications can still dispatch.
    const DWORD start = ::GetTickCount();
    bool got_quit = false;
    for (;;) {
        const DWORD elapsed = ::GetTickCount() - start;
        if (elapsed >= timeout_ms_) {
            LH_LOG_WARN("DispatcherChunkProvider: FILE_CHUNK_REQUEST timeout");
            break;
        }
        const DWORD remaining = timeout_ms_ - elapsed;
        const DWORD wait_result = ::MsgWaitForMultipleObjects(
            1, &slot->event, FALSE, remaining, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    ::PostQuitMessage(static_cast<int>(msg.wParam));
                    got_quit = true;
                    break;
                }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            if (got_quit) break;
            // After the pump, our event may have been set by a
            // re-entrant DeliverChunk OR by CancelAll; re-check
            // under the lock.
            std::lock_guard<std::mutex> lock(mu_);
            if (slot->cancelled || slot->received) break;
            continue;
        }
        // WAIT_TIMEOUT, WAIT_FAILED, etc. — bail out.
        break;
    }

    // Detach THIS specific slot from the per-key queue under the lock.
    // A delivered slot was already popped by DeliverChunk, so this is a
    // no-op for it; a timed-out / cancelled / aborted slot is still queued
    // and must be removed here. Removing only our entry (vs erasing the
    // whole key) matters when sibling FetchChunk calls for the same
    // (transfer_id, file_id, offset) are queued — they must remain in
    // pending_ so the next DeliverChunk can find them.
    //
    // The event is closed under the same lock DeliverChunk / CancelAll
    // signal it under, so neither can SetEvent a HANDLE we have already
    // closed (and the kernel may have handed to someone else).
    bool received = false;
    bool canceled = false;
    std::vector<std::uint8_t> data;
    bool is_last = false;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(mu_);
        received = slot->received;
        canceled = slot->cancelled;
        data     = std::move(slot->data);
        is_last  = slot->is_last;
        error    = std::move(slot->error);
        EraseFromQueueLocked(key, slot);
        ::CloseHandle(slot->event);
        slot->event = nullptr;
    }

    if (got_quit) {
        return E_ABORT;
    }
    if (canceled && !received) {
        return STG_E_REVERTED;
    }
    if (!received) {
        return E_FAIL;  // timeout
    }
    if (!error.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "DispatcherChunkProvider: parent error: %.96s", error.c_str());
        LH_LOG_WARN(buf);
        return E_FAIL;
    }
    out_data    = std::move(data);
    out_is_last = is_last;
    return S_OK;
}

void DispatcherChunkProvider::DeliverChunk(const std::string&         transfer_id,
                                           const std::string&         file_id,
                                           std::uint64_t              offset,
                                           std::vector<std::uint8_t>  data,
                                           bool                       is_last,
                                           const std::string&         error) {
    const std::string key = MakeKey(transfer_id, file_id, offset);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(key);
    if (it == pending_.end() || it->second.empty()) {
        return;  // no waiter (timed out / canceled / unknown)
    }
    // FIFO: the oldest pending request wins. Match the wire order by
    // which FetchChunks were issued, which is also the order the parent
    // processes them. POP it here, under the lock: a reply consumes its
    // request. If the slot stayed queued until its waiter woke, a second
    // reply for the same tuple arriving before that could find the same
    // front slot again and overwrite the bytes the first waiter has not
    // read yet, stranding the second waiter until its timeout.
    std::shared_ptr<PendingChunk> slot = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) pending_.erase(it);

    slot->data     = std::move(data);
    slot->is_last  = is_last;
    slot->error    = error;
    slot->received = true;
    // Signal while still holding mu_. FetchChunk closes the event only
    // under the same lock, so the HANDLE cannot be closed (and its value
    // recycled by the kernel for something else) between our check and
    // the SetEvent.
    if (slot->event) ::SetEvent(slot->event);
}

void DispatcherChunkProvider::CancelAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [_, queue] : pending_) {
        for (auto& slot : queue) {
            if (slot) {
                slot->cancelled = true;
                // Signal under the lock for the same reason as in
                // DeliverChunk: the waiter closes its event under mu_.
                if (slot->event) ::SetEvent(slot->event);
            }
        }
    }
}

std::size_t DispatcherChunkProvider::PendingCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (const auto& [_, queue] : pending_) {
        n += queue.size();
    }
    return n;
}

std::string DispatcherChunkProvider::MakeKey(const std::string& transfer_id,
                                             const std::string& file_id,
                                             std::uint64_t      offset) {
    std::string key;
    key.reserve(transfer_id.size() + file_id.size() + 24);
    key.append(transfer_id);
    key.push_back('|');
    key.append(file_id);
    key.push_back('|');
    key.append(std::to_string(offset));
    return key;
}

void DispatcherChunkProvider::EraseFromQueueLocked(const std::string&                   key,
                                                   const std::shared_ptr<PendingChunk>& slot) {
    auto it = pending_.find(key);
    if (it == pending_.end()) return;
    auto& q = it->second;
    for (auto qit = q.begin(); qit != q.end(); ++qit) {
        if (*qit == slot) { q.erase(qit); break; }
    }
    if (q.empty()) pending_.erase(it);
}

}  // namespace leviathan::clipboard_helper
