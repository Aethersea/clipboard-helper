# clipboard-helper

Multi-platform helper processes (macOS and Windows) that act as system clipboard owners on behalf of the Leviathan Go server and the Shen Electron client.

## Platforms

- **macOS**: Swift 5.9 + AppKit (`NSApplication`). Handles `declareTypes:owner:` callbacks.
- **Windows**: C++17 + clang-cl. Manages OLE/COM clipboard operations in the interactive session.

## Architecture

Wire format: **4-byte little-endian length prefix + serialized protobuf** (`HelperMessage`).

- macOS uses Unix domain sockets (`AF_UNIX`).
- Windows uses Named Pipes (`\\.\pipe\leviathan-clipboard-{sessionId}`).

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
└── windows/                         # Windows implementation (C++17)
    ├── CMakeLists.txt
    ├── build.ps1
    ├── src/
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
