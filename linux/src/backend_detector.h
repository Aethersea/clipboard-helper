#pragma once
//
// backend_detector — decide which clipboard backend (Wayland / X11 /
// forced-XWayland) the helper should drive at startup.
//
// This module is intentionally pure: `DetectBackend` takes a `GetEnvFn`
// (any callable that maps an env-var name to its current value, or empty
// string if unset) and returns a `BackendDecision` describing what
// happened and what the caller should do next.
//
// The production wrapper `DetectBackendFromEnv()` calls the OS getenv;
// tests use lambdas that look values up in a fixed table so the rules
// can be exercised without touching the process environment.
//
// Why pure: the GNOME-vs-Wayland-vs-X11 selection is the highest-leverage
// piece of correctness — getting it wrong silently means the helper
// loses the clipboard on a whole desktop class. Easier to fence with
// unit tests than to debug through a live session.

#include <functional>
#include <string>
#include <string_view>

namespace leviathan::clipboard_helper {

enum class BackendKind {
    // Wayland native (KDE Plasma 6 via ext-data-control-v1, or wlroots
    // compositors via wlr-data-control-unstable-v1).
    Wayland,

    // X11 selection ownership via Qt's xcb QClipboard.
    X11,

    // GNOME Wayland session detected; helper must force
    // QT_QPA_PLATFORM=xcb to drive X11 over XWayland. Mutter does not
    // implement any data-control extension, so the native Wayland path
    // is unusable for a background clipboard agent.
    ForceXWayland,

    // No display server detected (no WAYLAND_DISPLAY, no DISPLAY).
    // Helper should exit non-zero.
    None,
};

struct BackendDecision {
    BackendKind kind;

    // Human-readable explanation of *why* this decision was made; logged
    // verbatim at INFO level by main.cpp so a session in the field can
    // be debugged from the helper's stderr without recompiling.
    std::string reason;

    // For ForceXWayland: the env var name + value to set before QPA
    // initialisation (typically "QT_QPA_PLATFORM" / "xcb"). Empty for
    // other kinds.
    std::string env_override_name;
    std::string env_override_value;
};

// Look up an environment variable. Returns the value, or an empty
// string when the variable is unset or empty. Tests pass a lambda that
// reads from a fixture; production passes `OsGetEnv`.
using GetEnvFn = std::function<std::string(std::string_view)>;

// Production env reader — wraps std::getenv. Empty string for unset.
std::string OsGetEnv(std::string_view name);

// Decide the backend from the env. Pure function: same inputs → same
// outputs, no I/O, no globals.
BackendDecision DetectBackend(const GetEnvFn& getenv);

// Convenience: DetectBackend(OsGetEnv). Apply the resulting env
// override to the live process environment (no-op if env_override_name
// is empty). Returns the decision so callers can log / branch on it.
BackendDecision DetectBackendAndApplyEnv();

// Second-pass decision: when DetectBackend initially picked
// BackendKind::Wayland but a runtime probe (see wayland_probe.h) found
// the compositor does not advertise ext-data-control / wlr-data-control,
// should the helper fall back to ForceXWayland (route through Qt's xcb
// QClipboard on XWayland) instead of dropping to the no-op stub?
//
// Returns true iff:
//   - wayland_has_data_control == false (the compositor can't drive a
//     background clipboard owner via either Wayland data-control
//     extension), AND
//   - have_display == true (XWayland is reachable, so the X11 backend
//     can actually take over).
//
// Pure: facts in, decision out. The caller (main.cpp) is responsible for
// running the probe and reading $DISPLAY; this function exists so the
// decision logic is unit-testable in isolation (all four input
// combinations) and lives in one named place rather than inlined in
// main.cpp where it would be invisible to the test surface.
bool ShouldFallbackToXWayland(bool wayland_has_data_control, bool have_display);

}  // namespace leviathan::clipboard_helper
