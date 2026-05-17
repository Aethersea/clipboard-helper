#!/usr/bin/env bash
#
# fetch-protocols.sh — download Wayland protocol XMLs from upstream
# into this directory. One-time setup; rerun to update pinned commits.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

# Pinned to a known-good wlr-protocols commit. Bump deliberately
# (verify the XML still parses + compositor-side compat stays intact
# on Sway / Hyprland / Plasma 6.x).
WLR_PROTOCOLS_REV="2b8d43325b7012cc544f9af7ec47c1ad27d1df70"

# Pinned to wayland-protocols 1.39 (Dec 2024) — first release with
# ext-data-control-v1 in staging. Bump when newer protocols stabilize.
WAYLAND_PROTOCOLS_REV="1.39"

fetch_one() {
    local out="$1"; shift
    local url="$1"; shift
    echo "→ $out"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --silent --show-error --location --output "$out" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget --quiet --output-document "$out" "$url"
    else
        echo "Neither curl nor wget found; install one and rerun." >&2
        exit 1
    fi
}

fetch_one \
    "wlr-data-control-unstable-v1.xml" \
    "https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/${WLR_PROTOCOLS_REV}/unstable/wlr-data-control-unstable-v1.xml"

fetch_one \
    "ext-data-control-v1.xml" \
    "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/${WAYLAND_PROTOCOLS_REV}/staging/ext-data-control/ext-data-control-v1.xml"

echo
echo "Done. Protocol XMLs are now in $script_dir/."
echo "You can re-run cmake configure now."
