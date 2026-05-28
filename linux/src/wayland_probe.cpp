#include "wayland_probe.h"

#include <cstdint>
#include <cstring>

#include <wayland-client.h>

#include "log.h"

namespace leviathan::clipboard_helper {

namespace {

struct ProbeState {
    bool has_ext = false;
    bool has_wlr = false;
};

void OnRegistryGlobal(void* data, ::wl_registry* /*registry*/,
                      std::uint32_t /*name*/, const char* interface,
                      std::uint32_t /*version*/) {
    if (interface == nullptr) return;
    auto* s = static_cast<ProbeState*>(data);
    // Two interface names are the only ones we care about; anything else
    // we ignore. No bind / no version check — presence in the registry
    // is sufficient for the "can the compositor do data-control" answer.
    if (std::strcmp(interface, "ext_data_control_manager_v1") == 0) {
        s->has_ext = true;
    } else if (std::strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        s->has_wlr = true;
    }
}

void OnRegistryGlobalRemove(void* /*data*/, ::wl_registry* /*registry*/,
                            std::uint32_t /*name*/) {
    // Probes are short-lived; nothing to do on remove.
}

const ::wl_registry_listener kRegistryListener = {
    /*global=*/        &OnRegistryGlobal,
    /*global_remove=*/ &OnRegistryGlobalRemove,
};

}  // namespace

bool ProbeWaylandDataControl() {
    // wl_display_connect(NULL) honours WAYLAND_DISPLAY / WAYLAND_SOCKET.
    // Returns null if WAYLAND_DISPLAY is unset or the socket isn't reachable
    // — both are valid "no data-control here" answers.
    ::wl_display* display = ::wl_display_connect(nullptr);
    if (display == nullptr) {
        LH_LOG_DEBUG("[wayland-probe] wl_display_connect failed");
        return false;
    }

    ::wl_registry* registry = ::wl_display_get_registry(display);
    if (registry == nullptr) {
        ::wl_display_disconnect(display);
        return false;
    }

    ProbeState state;
    ::wl_registry_add_listener(registry, &kRegistryListener, &state);

    // Drives the registry global events to completion.  Negative return
    // signals a connection error mid-roundtrip; treat as "no answer" and
    // bail (caller falls back as if data-control were absent).
    if (::wl_display_roundtrip(display) < 0) {
        LH_LOG_DEBUG("[wayland-probe] wl_display_roundtrip failed");
        ::wl_registry_destroy(registry);
        ::wl_display_disconnect(display);
        return false;
    }

    ::wl_registry_destroy(registry);
    ::wl_display_disconnect(display);
    return state.has_ext || state.has_wlr;
}

}  // namespace leviathan::clipboard_helper
