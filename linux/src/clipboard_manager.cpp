#include "clipboard_manager.h"

#include <mutex>
#include <sstream>
#include <utility>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

class StubClipboardManager : public ClipboardManager {
public:
    void SetClipboardText(const std::string& utf8) override {
        std::ostringstream m;
        m << "[stub] SetClipboardText(len=" << utf8.size() << ")";
        LH_LOG_INFO(m.str());
    }

    void SetClipboardImage(const std::vector<std::uint8_t>& webp_bytes) override {
        std::ostringstream m;
        m << "[stub] SetClipboardImage(len=" << webp_bytes.size() << ")";
        LH_LOG_INFO(m.str());
    }

    void AnnounceDelayedText(const std::string& content_hash) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            pending_hash_ = content_hash;
        }
        std::ostringstream m;
        m << "[stub] AnnounceDelayedText(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());
    }

    void AnnounceDelayedImage(const std::string& content_hash) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            pending_hash_ = content_hash;
        }
        std::ostringstream m;
        m << "[stub] AnnounceDelayedImage(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());
    }

    void AnnounceDelayedFiles(const std::string& content_hash) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            pending_hash_ = content_hash;
        }
        std::ostringstream m;
        m << "[stub] AnnounceDelayedFiles(hash=" << ShortHash(content_hash) << ")";
        LH_LOG_INFO(m.str());
    }

    void ProvideData(const std::string&            content_hash,
                     const std::vector<std::uint8_t>& data) override {
        std::string expected;
        {
            std::lock_guard<std::mutex> g(mu_);
            expected = pending_hash_;
        }
        if (expected != content_hash) {
            std::ostringstream m;
            m << "[stub] ProvideData hash mismatch (pending="
              << ShortHash(expected) << " received=" << ShortHash(content_hash)
              << "); discarding " << data.size() << " bytes";
            LH_LOG_WARN(m.str());
            return;
        }
        std::ostringstream m;
        m << "[stub] ProvideData(hash=" << ShortHash(content_hash)
          << ", len=" << data.size() << ")";
        LH_LOG_INFO(m.str());
    }

    std::optional<std::string> GetClipboardText() override {
        LH_LOG_DEBUG("[stub] GetClipboardText -> nullopt");
        return std::nullopt;
    }

    void SetOnClipboardChanged(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(mu_);
        on_changed_ = std::move(cb);
    }

    void SetOnDataRequest(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> g(mu_);
        on_request_ = std::move(cb);
    }

private:
    // Compact the SHA-256 hex hash for logging — first 8 chars is
    // enough to distinguish hashes in a session log and matches the
    // truncation the macOS / Windows / Rust sides use.
    static std::string ShortHash(const std::string& h) {
        if (h.size() <= 8) return h;
        return h.substr(0, 8) + "...";
    }

    std::mutex                                       mu_;
    std::string                                      pending_hash_;
    std::function<void(const std::string&)>          on_changed_;
    std::function<void(const std::string&)>          on_request_;
};

}  // namespace

std::unique_ptr<ClipboardManager> MakeStubManager() {
    return std::make_unique<StubClipboardManager>();
}

// Umbrella Wayland factory. Prefer ext-data-control-v1 (the only
// protocol KDE Plasma 6.5+ supports); fall back to
// wlr-data-control-unstable-v1 (Sway, Hyprland, Plasma 6.0–6.4 and
// other wlroots-based compositors that haven't adopted ext yet).
// Returns nullptr if neither backend can initialise; main.cpp then
// drops to a stub manager.
std::unique_ptr<ClipboardManager> MakeWaylandManager() {
    if (auto m = MakeExtManager()) {
        LH_LOG_INFO("[wayland] using ext_data_control_v1 backend");
        return m;
    }
    LH_LOG_INFO("[wayland] ext_data_control_v1 unavailable; trying zwlr_data_control_unstable_v1");
    if (auto m = MakeWlrManager()) {
        LH_LOG_INFO("[wayland] using zwlr_data_control_unstable_v1 backend");
        return m;
    }
    LH_LOG_ERROR("[wayland] no data-control backend available");
    return nullptr;
}

}  // namespace leviathan::clipboard_helper
