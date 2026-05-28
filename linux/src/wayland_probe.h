#pragma once
//
// wayland_probe — answer the single question "does this Wayland compositor
// advertise ext_data_control_manager_v1 OR zwlr_data_control_manager_v1?"
// using only libwayland-client (no Qt, no QGuiApplication).
//
// Why standalone: main.cpp needs the answer BEFORE it constructs
// QGuiApplication, because Qt's QPA platform plugin is locked at app
// construction. If we discover the Wayland session can't actually drive
// the clipboard, we want to switch to QT_QPA_PLATFORM=xcb (route through
// XWayland) instead of QT_QPA_PLATFORM=minimal — that decision must land
// before QGuiApplication is built. wayland_display.* can't be used here
// because it installs a QSocketNotifier and needs the Qt event loop.
//
// The probe does its own `wl_display_connect` + registry roundtrip +
// disconnect; the helper proper later reconnects via WaylandDisplay if
// the Wayland backend ends up being kept.

namespace leviathan::clipboard_helper {

// Returns true iff a wl_display can be reached and its registry advertises
// at least one of: `ext_data_control_manager_v1`,
// `zwlr_data_control_manager_v1`. Returns false on any failure (no
// WAYLAND_DISPLAY, connect failure, allocation failure, etc.) — caller
// treats false as "no data-control here, consider falling back".
bool ProbeWaylandDataControl();

}  // namespace leviathan::clipboard_helper
