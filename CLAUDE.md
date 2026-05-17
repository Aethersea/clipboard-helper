# clipboard-helper

Multi-platform helper processes (macOS, Windows, Linux) that act as system clipboard owners on behalf of the Leviathan Go server and the Shen Electron client.

## Platforms

- **macOS**: Swift 5.9 + AppKit (`NSApplication`). Handles `declareTypes:owner:` callbacks.
- **Windows**: C++17 + clang-cl. Manages OLE/COM clipboard operations in the interactive session.
- **Linux**: C++17 + Qt 6 (`QCoreApplication` + later `QClipboard` / `QMimeData::retrieveData`). Runtime-detects Wayland (`ext-data-control-v1` for KDE Plasma 6, `wlr-data-control-unstable-v1` for wlroots compositors), X11 (Qt `xcb`), or GNOME (forced to XWayland via `QT_QPA_PLATFORM=xcb` — Mutter doesn't implement any data-control extension). **P1 skeleton landed**; Wayland/X11 backends stubbed pending follow-up PR.

## Architecture

Wire format: **4-byte little-endian length prefix + serialized protobuf** (`HelperMessage`).

- macOS uses Unix domain sockets (`AF_UNIX`).
- Windows uses Named Pipes (`\\.\pipe\leviathan-clipboard-{sessionId}`).
- Linux uses Unix domain sockets (`AF_UNIX`), `SOCK_STREAM` with mode `0600`; path under `$XDG_RUNTIME_DIR` (fallback `/tmp`).

## Build Commands

### macOS
```bash
make -C macos build             # swift build (debug)
make -C macos build-release     # swift build -c release
make -C macos build-universal   # arm64 + x86_64 universal binary
make -C macos generate-proto    # regenerate Swift from .proto
make -C macos clean             # clean build artifacts
```

### Windows
See [windows/README.md](windows/README.md) for detailed prerequisites and build instructions.
```pwsh
# Basic build
pwsh -File windows/build.ps1 -Config Release
```

### Linux
See [linux/README.md](linux/README.md). Requires Qt 6.5+ (`apt install qt6-base-dev` / `dnf install qt6-qtbase-devel`).
```bash
cd linux && ./build.sh                  # Release build + tests
cd linux && ./build.sh debug            # Debug build + tests
cd linux && ./build.sh release notest   # Release build, skip tests
```

## Project Structure

```
clipboard-helper/
├── Proto/                           # Shared .proto schema (Source of Truth)
│   ├── clipboard.proto
│   └── clipboard_helper.proto
├── macos/                           # macOS implementation (Swift)
│   ├── Package.swift
│   ├── Makefile
│   ├── Scripts/generate-proto.sh
│   └── Sources/ClipboardHelper/
├── windows/                         # Windows implementation (C++17)
│   ├── CMakeLists.txt
│   ├── build.ps1
│   ├── src/
│   └── README.md
└── linux/                           # Linux implementation (C++17 + Qt 6)
    ├── CMakeLists.txt
    ├── build.sh
    ├── src/
    │   ├── main.cpp                 # argparse, QCoreApplication, wiring
    │   ├── socket_server.cpp/.h     # AF_UNIX listener + framing + worker thread
    │   ├── proto_wire.cpp/.h        # hand-rolled varint + length-delim (copied from windows/)
    │   ├── dispatch.cpp/.h          # HelperMessage routing → ClipboardManager
    │   ├── dispatch_codec.cpp/.h    # encode/decode (independently unit-testable)
    │   ├── clipboard_manager.cpp/.h # Qt QClipboard wrapper [STUB in P1]
    │   ├── backend_detector.cpp/.h  # env var → Wayland/X11/GNOME-XWayland
    │   ├── parent_watchdog.cpp/.h   # prctl PR_SET_PDEATHSIG + /proc poll
    │   ├── log.cpp/.h               # stderr logger
    │   ├── helper_proto.h           # HelperMessage enums + field numbers
    │   └── *.cpp/.h
    ├── tests/                       # test_lite.h (header-only) + per-module tests
    └── README.md
```

## Proto Definitions

### `clipboard.proto` — Shared data types
| Message | Purpose |
|---|---|
| `ClipboardData` | Clipboard content: `content_type`, `payload`, `content_hash`, `files[]`, `transfer_id` |
| `ClipboardContentType` | Enum: `TEXT`, `IMAGE`, `FILES` |
| `FileMetadata` | Per-file info: `file_id`, `filename`, `relative_path`, `file_size`, `mime_type`, `checksum_sha256` |
| `ClipboardAnnouncement` | Metadata-only announcement for delayed rendering |
| `ClipboardDataRequest` | Sent by helper when OS triggers a paste |

### `clipboard_helper.proto` — IPC envelope
| Direction | Message Types |
|---|---|
| Parent → Helper | `SET_CLIPBOARD`, `ANNOUNCE_DELAYED`, `PROVIDE_DATA`, `GET_CLIPBOARD`, `SHUTDOWN`, `FILE_TRANSFER_PROGRESS` |
| Helper → Parent | `CLIPBOARD_CHANGED`, `DATA_REQUEST`, `CLIPBOARD_CONTENT`, `ERROR`, `READY` |
| Bidirectional | `FILE_CHUNK_REQUEST`, `FILE_CHUNK_DATA` |

## Regenerating Protobuf

When `Proto/*.proto` files change, run `make -C macos generate-proto`. This requires `protoc` and the `swift-protobuf` plugin installed. Generated files are committed to the repo.
