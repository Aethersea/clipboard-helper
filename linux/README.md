# clipboard-helper — Linux

Qt 6 / C++17 implementation of the `clipboard-helper` agent for Linux. Sibling to `macos/` (Swift + AppKit) and `windows/` (C++17 + clang-cl).

Speaks the same length-prefixed `HelperMessage` protobuf wire protocol; runtime-selects between **Wayland** (`ext-data-control-v1` for KDE Plasma 6, `wlr-data-control-unstable-v1` for wlroots compositors), **X11** (Qt `xcb` QClipboard), and **GNOME XWayland** (force `QT_QPA_PLATFORM=xcb` because Mutter does not implement any data-control extension).

> **Status — P3 complete (TEXT + IMAGE + FILES)**: IPC layer, dispatch, parent watchdog, backend detector all in. **Wayland (`wlr-data-control` + `ext-data-control`) and X11 backends handle all three content types in delayed rendering.**

The IMAGE path decodes the lossless WebP shen sends on the wire via QImage (qt6-image-formats-plugins) and re-encodes per paste consumer's MIME (PNG / BMP / Qt-native QImage).

The FILES path mirrors the macOS helper's flow: shen downloads files locally and returns newline-joined absolute paths via PROVIDE_DATA; the helper formats those into `file://` URIs joined by CRLF per RFC 2483 and writes them to the paste consumer's fd as `text/uri-list`.

## Architecture

```
shen / leviathan (parent)
        │
        │ AF_UNIX SOCK_STREAM, length-prefixed protobuf
        ▼
clipboard-helper
  ├─ socket_server (worker thread, accept + frame I/O)
  ├─ dispatch (HelperMessage routing)
  ├─ clipboard_manager (Qt event loop, QClipboard + DelayedMimeData)
  │     ├─ Wayland backend  (ext-data-control / wlr-data-control)  [stub]
  │     └─ X11 backend       (Qt xcb QClipboard)                    [stub]
  ├─ backend_detector (env vars → backend choice)
  └─ parent_watchdog (prctl PR_SET_PDEATHSIG + /proc/<pid> poll)
```

## Build prerequisites

| Dependency | Floor | Debian/Ubuntu | Fedora |
|---|---|---|---|
| CMake | 3.21 | `cmake` | `cmake` |
| clang or gcc | clang 16+ / gcc 12+ | `clang` | `clang` |
| Qt 6 (Core + Gui) | 6.5 LTS | `qt6-base-dev` | `qt6-qtbase-devel` |
| Qt 6 image format plugins (for WebP decode) | 6.5 LTS | `qt6-image-formats-plugins` | `qt6-qtimageformats` |

Wayland backend dependencies (added in the next PR alongside the actual binding):

| Dependency | Purpose |
|---|---|
| `libwayland-client` | Wayland protocol I/O |
| `wayland-protocols ≥ 1.32` | provides `wlr-data-control-unstable-v1.xml` |
| `wayland-scanner` | code-gen for protocol XML → client stubs |

## Build

```bash
cd clipboard-helper/linux
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
ctest --output-on-failure      # unit tests
```

Or via the wrapper:

```bash
./build.sh           # release build + tests
./build.sh debug     # debug build + tests
./build.sh notest    # skip tests
```

The binary lands at `build/clipboard-helper`.

## Run

```bash
./build/clipboard-helper --socket /tmp/clipboard-helper.sock [--mode server|client] [--parent-pid <pid>] [--verbose]
```

- `--socket` (required): AF_UNIX path the helper listens on
- `--mode` (default: `client`): matches the macOS helper convention
- `--parent-pid`: exit when the named PID dies (uses `prctl(PR_SET_PDEATHSIG, SIGTERM)` plus a `/proc/<pid>/stat` poll for the edge case where PDEATHSIG races on thread-leader changes)
- `--verbose`: enable DEBUG-level logs to stderr

## Backend selection

On startup the helper logs which backend it picked. Selection rules (`backend_detector.cpp`):

1. `XDG_CURRENT_DESKTOP` contains `GNOME` / `Unity` → force XWayland (set `QT_QPA_PLATFORM=xcb`), even in a Wayland session. **Why**: Mutter does not implement `wlr-data-control-unstable-v1` or `ext-data-control-v1`, so the native Wayland path is unusable for a background clipboard agent.
2. Else `WAYLAND_DISPLAY` is set → Wayland backend
3. Else `DISPLAY` is set → X11 backend
4. Otherwise exit non-zero with a clear error to stderr

## Cross-platform invariants

Inherited from `macos/` and `windows/` — these are NOT negotiable:

1. **Helper-side wait MUST ≥ parent-side wait**. Late `PROVIDE_DATA` would otherwise leak signals into the next render request.
2. **`content_hash` is strictly compared** on every `PROVIDE_DATA` against the current pending announcement. Mismatches are silently discarded.
3. **Drain wait conditions** when announcements get replaced or clipboard ownership is lost.

| Layer | text/image wait | files wait |
|---|---|---|
| Linux helper | 10 s | 300 s |
| Shen (parent) | 10 s | 120 s |

## Repo layout

```
linux/
├── CMakeLists.txt
├── README.md (this file)
├── build.sh
├── src/
│   ├── main.cpp                 # argparse, QCoreApplication, wiring
│   ├── log.cpp/.h               # stderr logger, level-gated
│   ├── proto_wire.cpp/.h        # hand-rolled varint + length-delim (copied from windows/)
│   ├── helper_proto.h           # HelperMessage enums + field numbers
│   ├── dispatch.cpp/.h          # HelperMessage routing
│   ├── dispatch_codec.cpp/.h    # encode/decode (independently testable)
│   ├── socket_server.cpp/.h     # AF_UNIX listener + framing + worker thread
│   ├── backend_detector.cpp/.h  # env var → backend choice
│   ├── parent_watchdog.cpp/.h   # prctl PR_SET_PDEATHSIG + /proc poll
│   └── clipboard_manager.cpp/.h # Qt QClipboard wrapper [stub in P1]
└── tests/
    ├── CMakeLists.txt
    ├── main.cpp
    ├── test_lite.h
    ├── proto_wire_test.cpp
    ├── dispatch_codec_test.cpp
    ├── socket_server_test.cpp
    ├── backend_detector_test.cpp
    └── log_test.cpp
```

## See also

- [`../macos/`](../macos/) — Swift + AppKit implementation
- [`../windows/`](../windows/) — C++17 + clang-cl implementation
- [`../Proto/`](../Proto/) — shared `.proto` schema (source of truth)
- Plan: `aethersea/clipboard-helper/linux-helper-plan.md` (local Obsidian vault)
- Tracking: [aethersea/clipboard-helper#1](https://git.reall.us/aethersea/clipboard-helper/-/issues/1)
