// WlrClipboardManager — drives the wlr-data-control-unstable-v1
// protocol via libwayland-client for background clipboard ownership.
//
// Threading model:
//   - All wl_* calls execute on the Qt main thread (the thread that
//     runs QCoreApplication::exec() and owns the WaylandDisplay's
//     QSocketNotifier). Public ClipboardManager methods can be called
//     from any thread (typically the SocketServer worker); they
//     marshal the actual Wayland calls onto the main thread via
//     QMetaObject::invokeMethod(Qt::QueuedConnection).
//
//   - The send-event handler spawns a detached std::thread per request.
//     That thread blocks on a condition variable waiting for the
//     parent's PROVIDE_DATA to arrive (or for a 10 s timeout), then
//     writes the bytes to the fd from the compositor and closes it.
//     Running it on a worker thread lets the Qt main thread keep
//     processing inbound IPC frames (the PROVIDE_DATA reply uses that
//     same main thread to update state).
//
//   - All cross-thread state (mutex, condvar, pending data, callbacks)
//     lives in a heap-allocated `WlrState` held via shared_ptr.
//     Detached worker threads capture the shared_ptr, NOT a raw `this`,
//     so the manager dying while a write/drain is in flight does not
//     UAF — workers see `stop` flip true and bail. Mirrors the X11
//     backend's DelayedState design.
//
// Lifecycle invariants matching the cross-platform contract:
//   1. wlr-data-control requires a FRESH zwlr_data_control_source_v1
//      per set_selection — reusing one is a fatal protocol error.
//   2. content_hash comparison on ProvideData: late / stale data is
//      silently discarded.
//   3. Replacing the announcement before send arrives → cancelled
//      event on the old source; we destroy it on cancel.

#include "clipboard_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "log.h"
#include "uri_list_format.h"
#include "wayland_display.h"

namespace leviathan::clipboard_helper {

namespace {

// What kind of content is currently announced / set. Determines which
// MIMEs we advertise on create_data_source AND how we materialise the
// bytes when the compositor's `send` event arrives.
enum class ContentKind { Text, Image, Files };

// MIME types we advertise for TEXT. Order matters: most-specific
// first per the convention pasters (LibreOffice in particular) use.
const char* const kTextMimeTypes[] = {
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain",
    "TEXT",
    "STRING",
};

// MIME types we advertise for IMAGE. The on-wire format from shen is
// lossless WebP; we decode via QImage and re-encode per request to
// whichever format the paste consumer asked for. PNG first (broadest
// modern app support); image/bmp second (older / Wine apps).
const char* const kImageMimeTypes[] = {
    "image/png",
    "image/bmp",
    "image/x-bmp",
};

// MIME types we advertise for FILES.
//
// - text/uri-list (RFC 2483): the universal standard every file
//   manager understands; CRLF-terminated file:// URIs.
// - x-special/gnome-copied-files: Nautilus / GTK convention that
//   carries an explicit copy-vs-cut hint. Format: first line is
//   "copy\n" or "cut\n" (no trailing newline), followed by URIs
//   joined with '\n' (NOT CRLF). Shen always sends copies, so we
//   hard-code "copy". Required for correct paste behaviour in
//   Nautilus / Files; without this MIME GNOME may treat the paste
//   ambiguously.
const char* const kFilesMimeTypes[] = {
    "text/uri-list",
    "x-special/gnome-copied-files",
};

constexpr std::chrono::seconds kProvideDataWait{10};

// Convert PROVIDE_DATA payload (newline-joined LOCAL absolute paths,
// the format the macOS helper's send_provide_data_file_paths produces
// on the Shen side) into the bytes a paste consumer expects for the
// MIME they requested (text/uri-list or x-special/gnome-copied-files).
// Pure: lives in uri_list_format.h so all three backends share one
// implementation + a single unit-test surface.
using uri_list::FormatForMime;

// Decode `webp_bytes` (or any QImage-readable format) and re-encode to
// the byte stream a paste consumer expects for `mime`. Empty result on
// any decode/encode failure — caller writes empty payload to the fd.
std::vector<std::uint8_t> ConvertImageForMime(
        const std::vector<std::uint8_t>& webp_bytes,
        const char* mime) {
    if (webp_bytes.empty() || mime == nullptr) return {};

    QImage img;
    if (!img.loadFromData(webp_bytes.data(),
                          static_cast<int>(webp_bytes.size()))) {
        LH_LOG_WARN("[wlr/image] QImage::loadFromData failed; "
                    "is qt6-image-formats-plugins installed?");
        return {};
    }

    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);

    // Map MIME → QImageWriter format name. We're conservative — only
    // support the formats we advertise. Anything else is a paster bug.
    const char* fmt = nullptr;
    if (std::strcmp(mime, "image/png") == 0)               fmt = "PNG";
    else if (std::strcmp(mime, "image/bmp") == 0
          || std::strcmp(mime, "image/x-bmp") == 0)        fmt = "BMP";

    if (fmt == nullptr) {
        std::ostringstream m;
        m << "[wlr/image] unsupported image MIME '" << mime
          << "'; advertised types should never reach this branch";
        LH_LOG_WARN(m.str());
        return {};
    }
    if (!img.save(&buf, fmt)) {
        std::ostringstream m;
        m << "[wlr/image] QImage::save as " << fmt << " failed";
        LH_LOG_WARN(m.str());
        return {};
    }
    return std::vector<std::uint8_t>(out.cbegin(), out.cend());
}

std::string ShortHash(const std::string& h) {
    if (h.size() <= 8) return h;
    return h.substr(0, 8) + "...";
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::write(fd, data + sent, len - sent);
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{ fd, POLLOUT, 0 };
            const int pr = ::poll(&p, 1, 5000);
            if (pr <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool DrainFd(int fd, std::vector<std::uint8_t>& out, std::size_t cap) {
    std::uint8_t buf[4096];
    while (out.size() < cap) {
        const ssize_t n = ::read(fd, buf,
            std::min(sizeof(buf), cap - out.size()));
        if (n > 0) {
            out.insert(out.end(), buf, buf + n);
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

// State shared between the manager (lives on the main thread) and the
// detached worker threads spawned for send / receive. Held via
// shared_ptr so workers in flight when the manager is destroyed still
// have a valid object to dereference — they observe `stop=true` and
// bail without touching the manager itself.
struct WlrState {
    std::mutex                                       mu;
    std::condition_variable                          cv;
    std::atomic<bool>                                stop{false};
    std::string                                      current_hash;
    ContentKind                                      content_kind{ContentKind::Text};
    std::optional<std::vector<std::uint8_t>>         pending_data;
    std::string                                      pending_data_hash;
    std::optional<std::string>                       cached_clipboard_text;
    std::function<void(const std::string&)>          on_changed;
    std::function<void(const std::string&)>          on_request;
};

// Worker: wait for ProvideData matching `hash`, then write bytes to fd.
// `mime` tells us which representation to materialise (text payload
// pass-through, image WebP → PNG/BMP via QImage). Always closes fd.
// Captures only the shared state, no manager pointer.
void WaitAndWriteOnWorker(std::shared_ptr<WlrState> state,
                          int fd, std::string hash, std::string mime) {
    enum class Outcome { Stopping, Stale, Timeout, Wrote };
    Outcome outcome = Outcome::Timeout;
    std::vector<std::uint8_t> bytes;
    ContentKind               kind = ContentKind::Text;
    {
        std::unique_lock<std::mutex> lock(state->mu);
        const bool got_signal = state->cv.wait_for(lock, kProvideDataWait, [&] {
            return state->stop.load(std::memory_order_acquire)
                || (state->pending_data && state->pending_data_hash == hash);
        });

        if (state->stop.load(std::memory_order_acquire)) {
            outcome = Outcome::Stopping;
        } else if (got_signal && state->pending_data
                   && state->pending_data_hash == hash) {
            bytes = *state->pending_data;
            kind  = state->content_kind;
            outcome = Outcome::Wrote;
        } else if (got_signal) {
            // notify_all fired (e.g. a new announcement replaced our hash)
            // but the pending data doesn't match — distinct from a real
            // 10s timeout (which logs as Timeout below).
            outcome = Outcome::Stale;
        } else {
            outcome = Outcome::Timeout;
        }
    }

    switch (outcome) {
        case Outcome::Stopping:
            // Manager shutting down; quietly close.
            break;
        case Outcome::Stale: {
            std::ostringstream m;
            m << "[wlr] send for hash=" << ShortHash(hash)
              << " obsoleted by a newer announcement; closing fd empty";
            LH_LOG_INFO(m.str());
            break;
        }
        case Outcome::Timeout: {
            std::ostringstream m;
            m << "[wlr] PROVIDE_DATA timed out after "
              << kProvideDataWait.count() << "s for hash="
              << ShortHash(hash) << "; closing fd empty";
            LH_LOG_WARN(m.str());
            break;
        }
        case Outcome::Wrote: {
            if (kind == ContentKind::Image) {
                const auto converted = ConvertImageForMime(bytes, mime.c_str());
                if (!converted.empty()) {
                    WriteAll(fd, converted.data(), converted.size());
                } else {
                    // Conversion failed (decode/encode error, unsupported
                    // MIME). Empty write so the paster sees an empty
                    // result rather than corrupted bytes.
                }
            } else if (kind == ContentKind::Files) {
                const auto uris = FormatForMime(bytes, mime.c_str());
                WriteAll(fd, uris.data(), uris.size());
            } else {
                WriteAll(fd, bytes.data(), bytes.size());
            }
            break;
        }
    }
    ::close(fd);
}

// Worker: drain a pipe fd that holds incoming clipboard data, then
// dispatch the resulting text to on_changed and cache it. Always
// closes fd.
void DrainAndDispatchOnWorker(std::shared_ptr<WlrState> state, int fd) {
    std::vector<std::uint8_t> buf;
    // Cap at 16 MiB to match the IPC frame size cap.
    const bool ok = DrainFd(fd, buf, 16 * 1024 * 1024);
    ::close(fd);

    if (state->stop.load(std::memory_order_acquire)) return;
    if (!ok) return;

    std::string text(buf.begin(), buf.end());
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> g(state->mu);
        state->cached_clipboard_text = text;
        cb = state->on_changed;
    }
    if (cb) cb(text);
}

}  // anonymous namespace

// ── Manager implementation ────────────────────────────────────────────
//
// Lives in the surrounding leviathan::clipboard_helper namespace (not
// the anonymous one) so MakeWlrManager — the public entry point
// declared in clipboard_manager.h — can name the type.

class WlrClipboardManager : public QObject, public ClipboardManager {
public:
    WlrClipboardManager() : state_(std::make_shared<WlrState>()) {}

    ~WlrClipboardManager() override {
        // Wake any detached workers. They hold a shared_ptr<WlrState>
        // so the state itself outlives us; the manager's wl_* objects
        // are destroyed below and the workers do NOT touch them.
        state_->stop.store(true, std::memory_order_release);
        state_->cv.notify_all();

        // Wayland-side teardown MUST happen on the main thread. We
        // assert main-thread destruction rather than marshal blocking,
        // because BlockingQueuedConnection can deadlock if the main
        // thread is itself stuck.
        assert(QCoreApplication::instance() == nullptr
               || QThread::currentThread()
                  == QCoreApplication::instance()->thread());
        TeardownWaylandObjects();
        display_.reset();
    }

    bool Init() {
        display_ = std::make_unique<WaylandDisplay>(this);
        if (!display_->Start()) {
            display_.reset();
            return false;
        }
        if (display_->manager_wlr() == nullptr) {
            LH_LOG_DEBUG("[wlr] compositor did not advertise zwlr_data_control_manager_v1");
            display_.reset();
            return false;
        }

        device_ = zwlr_data_control_manager_v1_get_data_device(
            display_->manager_wlr(), display_->seat());
        if (device_ == nullptr) {
            LH_LOG_ERROR("zwlr_data_control_manager_v1_get_data_device returned null");
            display_.reset();
            return false;
        }
        zwlr_data_control_device_v1_add_listener(
            device_, &kDeviceListener, this);
        display_->Flush();
        return true;
    }

    // ── ClipboardManager overrides (called from any thread) ──

    void SetClipboardText(const std::string& utf8) override {
        std::ostringstream m;
        m << "[wlr] SetClipboardText(len=" << utf8.size() << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            // hash unused for eager set; treat the payload itself as the
            // matching key for ProvideData hash comparison.
            state_->current_hash      = utf8;
            state_->content_kind      = ContentKind::Text;
            state_->pending_data      = std::vector<std::uint8_t>(utf8.begin(), utf8.end());
            state_->pending_data_hash = utf8;
        }
        state_->cv.notify_all();
        InvokeOnMain([this] { RecreateSource(ContentKind::Text); });
    }

    void SetClipboardImage(const std::vector<std::uint8_t>& webp_bytes) override {
        std::ostringstream m;
        m << "[wlr] SetClipboardImage(len=" << webp_bytes.size() << ")";
        LH_LOG_INFO(m.str());

        // Same eager-payload trick as SetClipboardText: we don't carry
        // a separate hash on the wire for SET_CLIPBOARD, so the bytes
        // double as both the payload and the dedup key.
        std::string key(webp_bytes.begin(), webp_bytes.end());
        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash      = key;
            state_->content_kind      = ContentKind::Image;
            state_->pending_data      = webp_bytes;
            state_->pending_data_hash = key;
        }
        state_->cv.notify_all();
        InvokeOnMain([this] { RecreateSource(ContentKind::Image); });
    }

    void AnnounceDelayedText(const std::string& content_hash) override {
        std::ostringstream m;
        m << "[wlr] AnnounceDelayedText(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash = content_hash;
            state_->content_kind = ContentKind::Text;
            state_->pending_data.reset();
            state_->pending_data_hash.clear();
        }
        // Wake blocked workers so they observe the replaced hash and
        // exit via the Stale path (writing an empty payload).
        state_->cv.notify_all();

        InvokeOnMain([this] { RecreateSource(ContentKind::Text); });
    }

    void AnnounceDelayedImage(const std::string& content_hash) override {
        std::ostringstream m;
        m << "[wlr] AnnounceDelayedImage(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash = content_hash;
            state_->content_kind = ContentKind::Image;
            state_->pending_data.reset();
            state_->pending_data_hash.clear();
        }
        state_->cv.notify_all();
        InvokeOnMain([this] { RecreateSource(ContentKind::Image); });
    }

    void AnnounceDelayedFiles(const std::string& content_hash) override {
        std::ostringstream m;
        m << "[wlr] AnnounceDelayedFiles(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());

        {
            std::lock_guard<std::mutex> g(state_->mu);
            state_->current_hash = content_hash;
            state_->content_kind = ContentKind::Files;
            state_->pending_data.reset();
            state_->pending_data_hash.clear();
        }
        state_->cv.notify_all();
        InvokeOnMain([this] { RecreateSource(ContentKind::Files); });
    }

    void ProvideData(const std::string&            content_hash,
                     const std::vector<std::uint8_t>& data) override {
        {
            std::lock_guard<std::mutex> g(state_->mu);
            if (state_->current_hash != content_hash) {
                std::ostringstream m;
                m << "[wlr] ProvideData hash mismatch (pending="
                  << ShortHash(state_->current_hash) << " received="
                  << ShortHash(content_hash) << "); discarding "
                  << data.size() << " bytes";
                LH_LOG_WARN(m.str());
                // Still notify so any waiter sees the mismatch (it'll
                // exit via the Stale path) instead of waiting the full
                // kProvideDataWait.
                state_->cv.notify_all();
                return;
            }
            state_->pending_data      = data;
            state_->pending_data_hash = content_hash;
        }
        state_->cv.notify_all();
    }

    std::optional<std::string> GetClipboardText() override {
        std::lock_guard<std::mutex> g(state_->mu);
        if (state_->cached_clipboard_text) return *state_->cached_clipboard_text;
        return std::nullopt;
    }

    void SetOnClipboardChanged(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->on_changed = std::move(cb);
    }

    void SetOnDataRequest(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(state_->mu);
        state_->on_request = std::move(cb);
    }

private:
    // ── Helpers ──

    template <typename F>
    void InvokeOnMain(F&& fn) {
        if (QCoreApplication::instance() == nullptr) return;
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            fn();
        } else {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                std::forward<F>(fn),
                Qt::QueuedConnection);
        }
    }

    void TeardownWaylandObjects() {
        // Destroy any tracked offers before tearing down the device
        // (they were created against the device's data_offer event).
        for (auto& kv : offer_mimes_) {
            zwlr_data_control_offer_v1_destroy(kv.first);
        }
        offer_mimes_.clear();
        if (current_source_ != nullptr) {
            zwlr_data_control_source_v1_destroy(current_source_);
            current_source_ = nullptr;
        }
        if (device_ != nullptr) {
            zwlr_data_control_device_v1_destroy(device_);
            device_ = nullptr;
        }
        if (display_) display_->Roundtrip();
    }

    void RecreateSource(ContentKind kind) {
        if (display_ == nullptr || device_ == nullptr) return;

        if (current_source_ != nullptr) {
            zwlr_data_control_source_v1_destroy(current_source_);
            current_source_ = nullptr;
        }

        current_source_ = zwlr_data_control_manager_v1_create_data_source(
            display_->manager_wlr());
        if (current_source_ == nullptr) {
            LH_LOG_ERROR("create_data_source returned null");
            return;
        }
        zwlr_data_control_source_v1_add_listener(
            current_source_, &kSourceListener, this);

        // Advertise the MIME set matching the announced content kind.
        // No mixing — a paste consumer can't ask for image bytes from
        // a text announcement (no MIME advertised) so the compositor
        // simply omits that consumer's data_offer.offer event for the
        // missing type.
        const char* const* mimes = nullptr;
        std::size_t        count = 0;
        switch (kind) {
            case ContentKind::Image:
                mimes = kImageMimeTypes;
                count = sizeof(kImageMimeTypes) / sizeof(kImageMimeTypes[0]);
                break;
            case ContentKind::Files:
                mimes = kFilesMimeTypes;
                count = sizeof(kFilesMimeTypes) / sizeof(kFilesMimeTypes[0]);
                break;
            case ContentKind::Text:
            default:
                mimes = kTextMimeTypes;
                count = sizeof(kTextMimeTypes) / sizeof(kTextMimeTypes[0]);
                break;
        }
        for (std::size_t i = 0; i < count; ++i) {
            zwlr_data_control_source_v1_offer(current_source_, mimes[i]);
        }

        zwlr_data_control_device_v1_set_selection(device_, current_source_);
        display_->Flush();
    }

    // ── wlr-data-control source events ──

    void OnSourceSend(const char* mime, int fd) {
        const std::string mime_str = (mime != nullptr) ? mime : "";
        std::string hash;
        ContentKind kind = ContentKind::Text;
        std::vector<std::uint8_t> eager_bytes;
        bool                      have_eager = false;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            hash = state_->current_hash;
            kind = state_->content_kind;
            if (state_->pending_data && state_->pending_data_hash == hash) {
                eager_bytes = *state_->pending_data;
                have_eager  = true;
            }
        }

        const char* kind_str = "text";
        if (kind == ContentKind::Image) kind_str = "image";
        else if (kind == ContentKind::Files) kind_str = "files";

        std::ostringstream lm;
        lm << "[wlr] send event (mime=" << (mime ? mime : "<null>")
           << ", fd=" << fd << ", hash=" << ShortHash(hash)
           << ", kind=" << kind_str
           << ", eager=" << (have_eager ? "yes" : "no") << ")";
        LH_LOG_DEBUG(lm.str());

        if (have_eager) {
            if (kind == ContentKind::Image) {
                const auto converted = ConvertImageForMime(eager_bytes, mime_str.c_str());
                if (!converted.empty()) {
                    WriteAll(fd, converted.data(), converted.size());
                }
                // else: empty write, paster sees no data (better than corrupted bytes).
            } else if (kind == ContentKind::Files) {
                const auto uris = FormatForMime(eager_bytes, mime_str.c_str());
                WriteAll(fd, uris.data(), uris.size());
            } else {
                WriteAll(fd, eager_bytes.data(), eager_bytes.size());
            }
            ::close(fd);
            return;
        }

        std::function<void(const std::string&)> req_cb;
        {
            std::lock_guard<std::mutex> g(state_->mu);
            req_cb = state_->on_request;
        }
        if (req_cb) req_cb(hash);

        // Worker captures the shared_ptr, NOT raw `this`; safe to
        // outlive the manager. Worker also receives the MIME so it
        // knows how to convert image payloads after they arrive.
        std::thread(WaitAndWriteOnWorker, state_, fd, hash, mime_str).detach();
    }

    void OnSourceCancelled() {
        InvokeOnMain([this] {
            if (current_source_ != nullptr) {
                zwlr_data_control_source_v1_destroy(current_source_);
                current_source_ = nullptr;
                LH_LOG_DEBUG("[wlr] source cancelled by compositor; destroyed");
            }
        });
    }

    // ── wlr-data-control device events ──

    void OnDeviceDataOffer(struct zwlr_data_control_offer_v1* offer) {
        if (offer == nullptr) return;
        zwlr_data_control_offer_v1_add_listener(offer, &kOfferListener, this);
        offer_mimes_[offer] = {};
    }

    void OnOfferMime(struct zwlr_data_control_offer_v1* offer,
                     const char* mime) {
        if (offer == nullptr || mime == nullptr) return;
        auto it = offer_mimes_.find(offer);
        if (it != offer_mimes_.end()) {
            it->second.insert(mime);
        }
    }

    // Destroy and erase every offer in `offer_mimes_` except `keep`
    // (which may be nullptr to destroy all). Returns the MIME set of
    // `keep` if it was tracked, empty otherwise.
    std::unordered_set<std::string> DrainOtherOffers(
            struct zwlr_data_control_offer_v1* keep) {
        std::unordered_set<std::string> keep_mimes;
        for (auto it = offer_mimes_.begin(); it != offer_mimes_.end();) {
            if (it->first == keep) {
                keep_mimes = it->second;
                ++it;
            } else {
                zwlr_data_control_offer_v1_destroy(it->first);
                it = offer_mimes_.erase(it);
            }
        }
        return keep_mimes;
    }

    void OnDeviceSelection(struct zwlr_data_control_offer_v1* offer) {
        // offer == nullptr → clipboard cleared by another client.
        if (offer == nullptr) {
            {
                std::lock_guard<std::mutex> g(state_->mu);
                state_->cached_clipboard_text.reset();
            }
            DrainOtherOffers(nullptr);
            return;
        }

        // Echo suppression: if we just took the clipboard via
        // set_selection, the compositor echoes a selection event back.
        // Don't recurse — but still destroy any straggling unselected
        // offers (the compositor may have introduced several before
        // this selection landed) so we don't leak them.
        const bool from_us = (current_source_ != nullptr);
        if (from_us) {
            DrainOtherOffers(/*keep=*/nullptr);  // includes `offer`
            return;
        }

        // Find this offer's MIME types AND destroy every other
        // outstanding offer (the compositor may have queued several
        // data_offer events before this selection — without the prune
        // they leak).
        const auto mimes = DrainOtherOffers(/*keep=*/offer);
        // `offer` itself is now the only entry left; remove it too
        // since we'll destroy it after issuing receive().
        offer_mimes_.erase(offer);

        const char* chosen = nullptr;
        for (const char* candidate : kTextMimeTypes) {
            if (mimes.count(candidate)) { chosen = candidate; break; }
        }
        if (chosen == nullptr) {
            // No text representation; ignore.
            zwlr_data_control_offer_v1_destroy(offer);
            return;
        }

        int pipefd[2];
        if (::pipe2(pipefd, O_CLOEXEC) != 0) {
            std::ostringstream m;
            m << "pipe2 failed: " << std::strerror(errno);
            LH_LOG_ERROR(m.str());
            zwlr_data_control_offer_v1_destroy(offer);
            return;
        }
        zwlr_data_control_offer_v1_receive(offer, chosen, pipefd[1]);
        ::close(pipefd[1]);
        if (display_) display_->Flush();

        zwlr_data_control_offer_v1_destroy(offer);

        // Worker captures shared_ptr<state>, not `this`.
        std::thread(DrainAndDispatchOnWorker, state_, pipefd[0]).detach();
    }

    // ── wlr-data-control listener tables (must be visible to C) ──

    static void S_SourceSend(void* data,
                             struct zwlr_data_control_source_v1*,
                             const char* mime, int32_t fd) {
        static_cast<WlrClipboardManager*>(data)->OnSourceSend(mime, fd);
    }
    static void S_SourceCancelled(void* data,
                                  struct zwlr_data_control_source_v1*) {
        static_cast<WlrClipboardManager*>(data)->OnSourceCancelled();
    }
    static const struct zwlr_data_control_source_v1_listener kSourceListener;

    static void S_DeviceDataOffer(void* data,
                                  struct zwlr_data_control_device_v1*,
                                  struct zwlr_data_control_offer_v1* offer) {
        static_cast<WlrClipboardManager*>(data)->OnDeviceDataOffer(offer);
    }
    static void S_DeviceSelection(void* data,
                                  struct zwlr_data_control_device_v1*,
                                  struct zwlr_data_control_offer_v1* offer) {
        static_cast<WlrClipboardManager*>(data)->OnDeviceSelection(offer);
    }
    static void S_DeviceFinished(void* data,
                                 struct zwlr_data_control_device_v1*) {
        (void)data;
        LH_LOG_WARN("[wlr] data_control_device finished by compositor");
    }
    static void S_DevicePrimarySelection(void* data,
                                         struct zwlr_data_control_device_v1*,
                                         struct zwlr_data_control_offer_v1* offer) {
        (void)data;
        if (offer != nullptr) {
            zwlr_data_control_offer_v1_destroy(offer);
        }
    }
    static const struct zwlr_data_control_device_v1_listener kDeviceListener;

    static void S_OfferMime(void* data,
                            struct zwlr_data_control_offer_v1* offer,
                            const char* mime) {
        static_cast<WlrClipboardManager*>(data)->OnOfferMime(offer, mime);
    }
    static const struct zwlr_data_control_offer_v1_listener kOfferListener;

    // ── State ──

    std::shared_ptr<WlrState>       state_;
    std::unique_ptr<WaylandDisplay>     display_;
    struct zwlr_data_control_device_v1* device_{nullptr};
    struct zwlr_data_control_source_v1* current_source_{nullptr};

    // Per-offer MIME accumulator; lifetime tied to the manager (only
    // touched from the main thread, no lock).
    std::unordered_map<struct zwlr_data_control_offer_v1*,
                       std::unordered_set<std::string>> offer_mimes_;
};

const struct zwlr_data_control_source_v1_listener
    WlrClipboardManager::kSourceListener = {
        /*send=*/      &WlrClipboardManager::S_SourceSend,
        /*cancelled=*/ &WlrClipboardManager::S_SourceCancelled,
};

const struct zwlr_data_control_device_v1_listener
    WlrClipboardManager::kDeviceListener = {
        /*data_offer=*/        &WlrClipboardManager::S_DeviceDataOffer,
        /*selection=*/         &WlrClipboardManager::S_DeviceSelection,
        /*finished=*/          &WlrClipboardManager::S_DeviceFinished,
        /*primary_selection=*/ &WlrClipboardManager::S_DevicePrimarySelection,
};

const struct zwlr_data_control_offer_v1_listener
    WlrClipboardManager::kOfferListener = {
        /*offer=*/ &WlrClipboardManager::S_OfferMime,
};

std::unique_ptr<ClipboardManager> MakeWlrManager() {
    auto m = std::make_unique<WlrClipboardManager>();
    if (!m->Init()) {
        LH_LOG_ERROR("WlrClipboardManager init failed");
        return nullptr;
    }
    return m;
}

}  // namespace leviathan::clipboard_helper
