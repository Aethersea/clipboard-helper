# Wayland protocol XMLs

The Linux clipboard helper drives Wayland clipboards via two non-core
extension protocols. Both must be present in this directory before CMake
configure can succeed; run [`fetch-protocols.sh`](fetch-protocols.sh) to
download pinned versions from the canonical upstream repositories.

| File | Source repo | Why pinned |
|---|---|---|
| `wlr-data-control-unstable-v1.xml` | [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) | Used by Sway, Hyprland, and KDE Plasma 6.0–6.4 to expose background clipboard ownership. |
| `ext-data-control-v1.xml` | [wayland-protocols ≥ 1.39](https://gitlab.freedesktop.org/wayland/wayland-protocols) | Successor to `wlr-data-control`, mandatory for **KDE Plasma 6.5+** (Plasma 6.5 began dropping `wlr-data-control`). The helper prefers this protocol when the compositor advertises it. |

## Why not vendor the XML?

`wlr-data-control-unstable-v1.xml` is small enough to ship in-tree, but
distros and CI environments expect the protocol XMLs to match the
versions the compositor on the same machine implements. Fetching from
upstream pinned to a known-good commit avoids the silent-divergence
problem where a stale vendored XML still parses but tickles
compositor-specific bugs.

## Why this isn't auto-run from CMake configure

CMake can't reach the network in air-gapped CI, and pulling a network
dependency on every `cmake -B build` is a footgun. The script is a
one-time setup step; CI is expected to either run it during job setup
or cache the downloaded XMLs as artifacts.

## Backend selection at runtime

`WaylandDisplay` binds both `ext_data_control_manager_v1` and
`zwlr_data_control_manager_v1` from the registry when they're
advertised by the compositor. `MakeWaylandManager()` (see
`src/clipboard_manager.h`) then picks:

1. `ext_data_control_v1` if available — preferred because Plasma 6.5+
   only ships this one.
2. `wlr_data_control_unstable_v1` otherwise — covers Sway, Hyprland,
   Plasma 6.0–6.4, and any wlroots-based compositor that hasn't
   adopted `ext-` yet.
3. Neither → `MakeWaylandManager` returns nullptr; `main.cpp` falls
   back to the no-op stub and logs a loud error.

The two protocols are wire-incompatible (different interface names
and opaque types) but structurally identical at the request/event
level. The implementation lives in two parallel files —
`clipboard_manager_wayland.cpp` (wlr) and `clipboard_manager_ext.cpp`
(ext) — that are intentionally near-copies so the diff stays
trivially auditable.
