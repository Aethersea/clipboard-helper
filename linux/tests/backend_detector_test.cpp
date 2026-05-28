// backend_detector unit tests — exercise the env-var → backend decision
// rules with synthetic GetEnvFn fixtures. The production wrapper
// DetectBackendAndApplyEnv() is not tested here because it mutates the
// real process environment; the rules themselves are pure.

#include "backend_detector.h"
#include "test_lite.h"

#include <map>
#include <string>

using leviathan::clipboard_helper::BackendDecision;
using leviathan::clipboard_helper::BackendKind;
using leviathan::clipboard_helper::DetectBackend;
using leviathan::clipboard_helper::GetEnvFn;
using leviathan::clipboard_helper::ShouldFallbackToXWayland;

namespace {

GetEnvFn MakeEnv(std::map<std::string, std::string> table) {
    return [table = std::move(table)](std::string_view name) -> std::string {
        auto it = table.find(std::string(name));
        if (it == table.end()) return {};
        return it->second;
    };
}

}  // namespace

// ─── Rule 1: GNOME forces XWayland ────────────────────────────────────────

TEST(BackendDetector, GnomeWaylandSessionForcesXWayland) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "GNOME"},
        {"WAYLAND_DISPLAY",     "wayland-0"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind),
              static_cast<int>(BackendKind::ForceXWayland));
    EXPECT_EQ(d.env_override_name, std::string("QT_QPA_PLATFORM"));
    EXPECT_EQ(d.env_override_value, std::string("xcb"));
    // The reason string should mention GNOME so a session log makes the
    // decision auditable.
    EXPECT_NE(d.reason.find("GNOME"), std::string::npos);
}

TEST(BackendDetector, GnomeIsCaseInsensitive) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "ubuntu:gnome"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind),
              static_cast<int>(BackendKind::ForceXWayland));
}

TEST(BackendDetector, GnomeClassicTriggersOverride) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "GNOME-Classic:GNOME"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind),
              static_cast<int>(BackendKind::ForceXWayland));
}

TEST(BackendDetector, UnityTriggersOverride) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "Unity"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind),
              static_cast<int>(BackendKind::ForceXWayland));
}

TEST(BackendDetector, GnomeWithNoDisplayReturnsNone) {
    // GNOME detected but no DISPLAY / WAYLAND_DISPLAY — can't reach any
    // display server, force-XWayland would fail anyway. Caller should
    // surface a clear error and exit non-zero.
    auto env = MakeEnv({{"XDG_CURRENT_DESKTOP", "GNOME"}});
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::None));
    EXPECT_TRUE(d.env_override_name.empty());
}

// ─── Rule 2: Wayland (non-GNOME) ──────────────────────────────────────────

TEST(BackendDetector, PlasmaWaylandUsesNativeWayland) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "KDE"},
        {"WAYLAND_DISPLAY",     "wayland-0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::Wayland));
    EXPECT_TRUE(d.env_override_name.empty());
}

TEST(BackendDetector, SwaySessionUsesNativeWayland) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "sway"},
        {"WAYLAND_DISPLAY",     "wayland-1"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::Wayland));
}

TEST(BackendDetector, HyprlandSessionUsesNativeWayland) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "Hyprland"},
        {"WAYLAND_DISPLAY",     "wayland-1"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::Wayland));
}

TEST(BackendDetector, WaylandPreferredOverX11WhenBothPresent) {
    // wayland-rooted compositors with XWayland set both DISPLAY (for
    // XWayland) and WAYLAND_DISPLAY. We should prefer the native path.
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "KDE"},
        {"WAYLAND_DISPLAY",     "wayland-0"},
        {"DISPLAY",             ":1"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::Wayland));
}

// ─── Rule 3: X11 ──────────────────────────────────────────────────────────

TEST(BackendDetector, X11OnlySessionUsesX11) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "XFCE"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::X11));
    EXPECT_TRUE(d.env_override_name.empty());
}

TEST(BackendDetector, KdeX11SessionUsesX11) {
    auto env = MakeEnv({
        {"XDG_CURRENT_DESKTOP", "KDE"},
        {"DISPLAY",             ":0"},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::X11));
}

// ─── Rule 4: nothing reachable ────────────────────────────────────────────

TEST(BackendDetector, NoDisplayEnvReturnsNone) {
    auto env = MakeEnv({});
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::None));
    EXPECT_TRUE(d.env_override_name.empty());
    EXPECT_FALSE(d.reason.empty());
}

TEST(BackendDetector, EmptyValuesAreIgnored) {
    // WAYLAND_DISPLAY="" should be treated as unset (covers `unset` vs
    // `export WAYLAND_DISPLAY=` discrepancies). Same for DISPLAY.
    auto env = MakeEnv({
        {"WAYLAND_DISPLAY", ""},
        {"DISPLAY",         ""},
    });
    auto d = DetectBackend(env);
    EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(BackendKind::None));
}

// ─── ShouldFallbackToXWayland — full truth table ────────────────────────────

TEST(ShouldFallbackToXWayland, FallsBackWhenNoDataControlAndDisplayAvailable) {
    // The canonical "rescuable" case: non-wlroots Wayland (Weston, COSMIC,
    // Mir, ...) lacking data-control, but XWayland is running so we can
    // route through Qt's xcb QClipboard.
    EXPECT_TRUE(ShouldFallbackToXWayland(/*has_data_control=*/false,
                                         /*have_display=*/true));
}

TEST(ShouldFallbackToXWayland, KeepsWaylandWhenDataControlAvailable) {
    // wlroots / KDE Plasma 6.5+ etc. — Wayland backend will work; do not
    // pre-empt it with XWayland even if DISPLAY happens to also be set.
    EXPECT_FALSE(ShouldFallbackToXWayland(/*has_data_control=*/true,
                                          /*have_display=*/true));
    EXPECT_FALSE(ShouldFallbackToXWayland(/*has_data_control=*/true,
                                          /*have_display=*/false));
}

TEST(ShouldFallbackToXWayland, KeepsWaylandWhenNoDisplay) {
    // Pure-Wayland session with no XWayland — fallback is impossible.
    // Caller logs the error and lets the Wayland branch drop to stub
    // (nothing else to do).
    EXPECT_FALSE(ShouldFallbackToXWayland(/*has_data_control=*/false,
                                          /*have_display=*/false));
}
