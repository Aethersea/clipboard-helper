#include "wayland_display.h"

#include <errno.h>
#include <string.h>

#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"

#include <sstream>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

const struct wl_registry_listener kRegistryListener = {
    /*global=*/        &WaylandDisplay::StaticRegistryGlobal,
    /*global_remove=*/ &WaylandDisplay::StaticRegistryGlobalRemove,
};

// Highest wl_seat version we know how to use. We don't actually need any
// seat events for clipboard ownership (data-control is seat-rooted but
// doesn't read keyboard/pointer state) — version 1 is enough but newer
// is fine too. Cap at 7 to match what compositors typically advertise.
constexpr std::uint32_t kMaxSeatVersion = 7;

// wlr-data-control v2 added primary_selection events; we currently
// don't read primary, but binding the higher version is forwards-
// compatible. ext-data-control-v1 currently only has version 1.
constexpr std::uint32_t kMaxWlrManagerVersion = 2;
constexpr std::uint32_t kMaxExtManagerVersion = 1;

std::uint32_t MinVersion(std::uint32_t advertised, std::uint32_t cap) {
    return advertised < cap ? advertised : cap;
}

}  // namespace

WaylandDisplay::WaylandDisplay(QObject* parent) : QObject(parent) {}

WaylandDisplay::~WaylandDisplay() {
    notifier_.reset();  // disconnect notifier before tearing fd down

    if (manager_ext_ != nullptr) {
        ext_data_control_manager_v1_destroy(manager_ext_);
        manager_ext_ = nullptr;
    }
    if (manager_wlr_ != nullptr) {
        zwlr_data_control_manager_v1_destroy(manager_wlr_);
        manager_wlr_ = nullptr;
    }
    if (seat_ != nullptr) {
        wl_seat_destroy(seat_);
        seat_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

bool WaylandDisplay::Start() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        std::ostringstream m;
        m << "wl_display_connect(NULL) failed (WAYLAND_DISPLAY="
          << (std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "<unset>")
          << "): " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    if (registry_ == nullptr) {
        LH_LOG_ERROR("wl_display_get_registry returned null");
        wl_display_disconnect(display_);
        display_ = nullptr;
        return false;
    }
    wl_registry_add_listener(registry_, &kRegistryListener, this);

    // Two roundtrips: first publishes the registry globals, second
    // ensures any seat-capability replies have landed before we
    // proceed. (We don't read seat capabilities but the second
    // roundtrip is cheap insurance.)
    if (wl_display_roundtrip(display_) < 0) {
        LH_LOG_ERROR("wl_display_roundtrip #1 failed");
        return false;
    }
    if (wl_display_roundtrip(display_) < 0) {
        LH_LOG_ERROR("wl_display_roundtrip #2 failed");
        return false;
    }

    if (seat_ == nullptr) {
        LH_LOG_ERROR(
            "Compositor did not advertise wl_seat — clipboard helper "
            "cannot bind a data-control device without one.");
        return false;
    }
    if (manager_wlr_ == nullptr && manager_ext_ == nullptr) {
        LH_LOG_ERROR(
            "Compositor advertised neither ext_data_control_manager_v1 "
            "nor zwlr_data_control_manager_v1. This usually means the "
            "session is GNOME / Mutter (which does not implement any "
            "data-control extension); the BackendDetector should have "
            "caught this and forced XWayland.");
        return false;
    }

    // wl_display_get_fd is stable for the connection's lifetime — safe
    // to hand off to QSocketNotifier without rebinding on each event.
    const int fd = wl_display_get_fd(display_);
    notifier_ = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read, this);
    connect(notifier_.get(), &QSocketNotifier::activated,
            this, &WaylandDisplay::OnReadable);

    // Flush any requests issued during Start() (registry binds count as
    // outbound traffic) so the compositor isn't stalled waiting for the
    // first event-driven dispatch.
    Flush();

    std::ostringstream ready;
    ready << "Wayland display ready (bound: "
          << (manager_ext_ ? "ext_data_control_v1 " : "")
          << (manager_wlr_ ? "zwlr_data_control_unstable_v1" : "")
          << ")";
    LH_LOG_INFO(ready.str());
    return true;
}

void WaylandDisplay::Roundtrip() {
    if (display_ != nullptr) {
        wl_display_roundtrip(display_);
    }
}

void WaylandDisplay::Flush() {
    if (display_ == nullptr) return;
    while (true) {
        const int n = wl_display_flush(display_);
        if (n >= 0) return;
        if (errno == EAGAIN) {
            // Socket buffer full; the outbound queue will drain when
            // the kernel makes room. The next QSocketNotifier::activated
            // call will retry the flush. Returning here means we don't
            // busy-loop on a write-blocked socket.
            return;
        }
        if (errno == EINTR) continue;
        std::ostringstream m;
        m << "wl_display_flush failed: " << std::strerror(errno);
        LH_LOG_ERROR(m.str());
        return;
    }
}

void WaylandDisplay::OnReadable() {
    if (display_ == nullptr) return;

    // We're the sole consumer of wl_display (no QPA plugin loaded in
    // QCoreApplication-only mode), so the prepare_read / read_events
    // pattern reduces to: drain any already-queued events, prepare a
    // read, do the read, dispatch what came in. The prepare_read loop
    // covers the case where dispatch_pending itself dispatches an
    // event that queues more events (re-entrancy).
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) {
            std::ostringstream m;
            m << "wl_display_dispatch_pending (pre-read) failed: "
              << std::strerror(errno);
            LH_LOG_ERROR(m.str());
            return;
        }
    }

    Flush();

    if (wl_display_read_events(display_) < 0) {
        if (errno != EAGAIN) {
            std::ostringstream m;
            m << "wl_display_read_events failed: " << std::strerror(errno);
            LH_LOG_ERROR(m.str());
        }
        return;
    }

    if (wl_display_dispatch_pending(display_) < 0) {
        std::ostringstream m;
        m << "wl_display_dispatch_pending (post-read) failed: "
          << std::strerror(errno);
        LH_LOG_ERROR(m.str());
    }

    Flush();
}

void WaylandDisplay::OnRegistryGlobal(::wl_registry* registry,
                                      std::uint32_t  name,
                                      const char*    interface,
                                      std::uint32_t  version) {
    if (std::strcmp(interface, wl_seat_interface.name) == 0 && seat_ == nullptr) {
        seat_name_ = name;
        const std::uint32_t use = MinVersion(version, kMaxSeatVersion);
        seat_ = static_cast<::wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, use));
        std::ostringstream m;
        m << "Bound wl_seat (name=" << name << ", version=" << use << ")";
        LH_LOG_DEBUG(m.str());
    } else if (std::strcmp(interface,
                           ext_data_control_manager_v1_interface.name) == 0
               && manager_ext_ == nullptr) {
        manager_ext_name_ = name;
        const std::uint32_t use = MinVersion(version, kMaxExtManagerVersion);
        manager_ext_ = static_cast<::ext_data_control_manager_v1*>(
            wl_registry_bind(registry, name,
                             &ext_data_control_manager_v1_interface, use));
        std::ostringstream m;
        m << "Bound ext_data_control_manager_v1 (name=" << name
          << ", version=" << use << ")";
        LH_LOG_DEBUG(m.str());
    } else if (std::strcmp(interface,
                           zwlr_data_control_manager_v1_interface.name) == 0
               && manager_wlr_ == nullptr) {
        manager_wlr_name_ = name;
        const std::uint32_t use = MinVersion(version, kMaxWlrManagerVersion);
        manager_wlr_ = static_cast<::zwlr_data_control_manager_v1*>(
            wl_registry_bind(registry, name,
                             &zwlr_data_control_manager_v1_interface, use));
        std::ostringstream m;
        m << "Bound zwlr_data_control_manager_v1 (name=" << name
          << ", version=" << use << ")";
        LH_LOG_DEBUG(m.str());
    }
}

void WaylandDisplay::OnRegistryGlobalRemove(std::uint32_t name) {
    // Globals can come and go (e.g. a display being hot-unplugged
    // removes its wl_output). The two globals we care about are
    // session-lifetime — losing them mid-session is exotic enough that
    // we log and let the broken pointer surface itself on the next
    // request rather than try to gracefully tear down.
    if (name == seat_name_ && seat_ != nullptr) {
        LH_LOG_WARN("wl_seat global removed mid-session; clipboard ownership likely broken");
    } else if (name == manager_wlr_name_ && manager_wlr_ != nullptr) {
        LH_LOG_WARN("zwlr_data_control_manager_v1 removed mid-session");
    } else if (name == manager_ext_name_ && manager_ext_ != nullptr) {
        LH_LOG_WARN("ext_data_control_manager_v1 removed mid-session");
    }
}

void WaylandDisplay::StaticRegistryGlobal(void* data, ::wl_registry* registry,
                                         std::uint32_t name,
                                         const char* interface,
                                         std::uint32_t version) {
    static_cast<WaylandDisplay*>(data)->OnRegistryGlobal(
        registry, name, interface, version);
}

void WaylandDisplay::StaticRegistryGlobalRemove(void* data, ::wl_registry* registry,
                                                std::uint32_t name) {
    (void)registry;
    static_cast<WaylandDisplay*>(data)->OnRegistryGlobalRemove(name);
}

}  // namespace leviathan::clipboard_helper
