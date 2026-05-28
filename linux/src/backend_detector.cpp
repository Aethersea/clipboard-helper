#include "backend_detector.h"

#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace leviathan::clipboard_helper {

namespace {

// Tokenise a colon-separated `XDG_CURRENT_DESKTOP` (per the XDG spec)
// into individual desktop tokens, upper-cased for case-insensitive
// matching. e.g. "ubuntu:GNOME" → {"UBUNTU", "GNOME"}.
std::vector<std::string> SplitAndUpper(std::string_view xdg) {
    std::vector<std::string> out;
    std::string token;
    for (char c : xdg) {
        if (c == ':') {
            if (!token.empty()) out.push_back(std::move(token));
            token.clear();
        } else {
            token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (!token.empty()) out.push_back(std::move(token));
    return out;
}

// GNOME's Mutter compositor is the only mainstream Wayland compositor
// that refuses to implement wlr-data-control / ext-data-control. Treat
// Unity (Wayland fork that inherits Mutter) the same way.
bool IsGnomeDesktop(std::string_view xdg_current_desktop) {
    const auto tokens = SplitAndUpper(xdg_current_desktop);
    for (const auto& t : tokens) {
        if (t == "GNOME" || t == "UNITY" || t == "GNOME-CLASSIC") {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string OsGetEnv(std::string_view name) {
    // std::getenv requires a NUL-terminated string; copy if not already.
    const std::string n(name);
    const char* v = std::getenv(n.c_str());
    if (v == nullptr) return {};
    return std::string(v);
}

BackendDecision DetectBackend(const GetEnvFn& getenv) {
    const std::string xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP");
    const std::string wayland_display     = getenv("WAYLAND_DISPLAY");
    const std::string x11_display         = getenv("DISPLAY");

    // Rule 1: GNOME detected → force XWayland regardless of session type.
    // We honour this even when WAYLAND_DISPLAY is unset (e.g. GNOME on X11)
    // because setting QT_QPA_PLATFORM=xcb is a no-op there anyway, and the
    // explicit-override path keeps the decision uniform in logs.
    if (IsGnomeDesktop(xdg_current_desktop)) {
        if (x11_display.empty() && wayland_display.empty()) {
            return {BackendKind::None,
                    "GNOME desktop detected but neither DISPLAY nor "
                    "WAYLAND_DISPLAY is set; cannot reach any display server",
                    {}, {}};
        }
        std::ostringstream r;
        r << "GNOME desktop ('" << xdg_current_desktop
          << "'); Mutter does not implement ext-data-control / "
             "wlr-data-control. Forcing QT_QPA_PLATFORM=xcb to route through "
             "XWayland.";
        return {BackendKind::ForceXWayland, r.str(),
                "QT_QPA_PLATFORM", "xcb"};
    }

    // Rule 2: WAYLAND_DISPLAY set → native Wayland.
    if (!wayland_display.empty()) {
        std::ostringstream r;
        r << "WAYLAND_DISPLAY=" << wayland_display
          << " set (XDG_CURRENT_DESKTOP='" << xdg_current_desktop
          << "'); using native Wayland backend (ext-data-control / "
             "wlr-data-control).";
        return {BackendKind::Wayland, r.str(), {}, {}};
    }

    // Rule 3: DISPLAY set → X11.
    if (!x11_display.empty()) {
        std::ostringstream r;
        r << "DISPLAY=" << x11_display
          << " set, no Wayland; using X11 backend (Qt xcb QClipboard).";
        return {BackendKind::X11, r.str(), {}, {}};
    }

    // Rule 4: nothing reachable.
    return {BackendKind::None,
            "Neither WAYLAND_DISPLAY nor DISPLAY is set; cannot reach any "
            "display server. Helper must run inside an interactive graphical "
            "session.",
            {}, {}};
}

bool ShouldFallbackToXWayland(bool wayland_has_data_control, bool have_display) {
    return !wayland_has_data_control && have_display;
}

BackendDecision DetectBackendAndApplyEnv() {
    auto decision = DetectBackend(OsGetEnv);
    if (!decision.env_override_name.empty()) {
        // setenv(name, value, /*overwrite=*/1) — we deliberately overwrite
        // any pre-existing value the user set, because our reason for
        // wanting xcb (GNOME data-control absence) is independent of what
        // the user might have configured.
        ::setenv(decision.env_override_name.c_str(),
                 decision.env_override_value.c_str(),
                 /*overwrite=*/1);
    }
    return decision;
}

}  // namespace leviathan::clipboard_helper
