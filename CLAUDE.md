# clipboard-helper

A macOS helper process that acts as the `NSPasteboard` owner on behalf of the Leviathan Go server and the Shen Electron client. It exists because `declareTypes:owner:` (lazy/delayed clipboard rendering) requires an active `NSApplication` run loop — without one, Universal Clipboard/Handoff immediately steals pasteboard ownership and deferred data callbacks never fire.

## Architecture

```
┌──────────────────┐   Unix Socket   ┌──────────────────────────┐
│  Go server       │ ◄──────────────► │  clipboard-helper (Swift) │
│  (leviathan)     │   protobuf IPC  │  - NSApplication run loop │
└──────────────────┘                  │  - NSPasteboard owner     │
┌──────────────────┐   Unix Socket   │  - delayed rendering      │
│  Electron client │ ◄──────────────► │  - pasteboard polling     │
│  (shen, Rust)    │   protobuf IPC  └──────────────────────────┘
└──────────────────┘
```

Wire format: **4-byte little-endian length prefix + serialized protobuf** (`HelperMessage`).

## Tech Stack

- **Language**: Swift 5.9
- **UI/App lifecycle**: AppKit (`NSApplication`, `NSPasteboard`)
- **CLI parsing**: `apple/swift-argument-parser` 1.3–1.4
- **Serialization**: Protocol Buffers via `apple/swift-protobuf` 1.28+
- **IPC transport**: Unix domain socket (`AF_UNIX`, `SOCK_STREAM`)
- **Min OS**: macOS 13 (Ventura)
- **Build system**: Swift Package Manager + `make`

## Build Commands

```bash
make build             # swift build (debug)
make build-release     # swift build -c release (current arch)
make build-universal   # arm64 + x86_64 lipo'd into .build/universal/ClipboardHelper
make generate-proto    # bash Scripts/generate-proto.sh (re-gen Swift from .proto)
make clean             # swift package clean && rm -rf .build
make install           # build-release then cp to /usr/local/bin/clipboard-helper
make run               # swift run ClipboardHelper --socket /tmp/clipboard-helper.sock --verbose
```

## CLI Usage

```bash
clipboard-helper --socket <path> [--mode server|client] [--verbose]
```

- `--mode server` (default): polls local pasteboard, reports changes upstream to the Go server
- `--mode client`: receives remote content from Electron/Shen, lazily renders to macOS pasteboard

## Project Structure

```
clipboard-helper/
├── Package.swift                    # SPM manifest — one executable target
├── Makefile
├── Proto/
│   ├── clipboard.proto              # Shared clipboard data types
│   └── clipboard_helper.proto       # IPC message envelope
├── Scripts/
│   └── generate-proto.sh            # Runs protoc → Sources/ClipboardHelper/Generated/
└── Sources/ClipboardHelper/
    ├── ClipboardHelperApp.swift      # @main entry point, AppDelegate, mode enum
    ├── SocketServer.swift            # Unix domain socket server (listen/accept/read/write)
    ├── PasteboardManager.swift       # NSPasteboard read/write/polling/delayed rendering
    ├── FileTransferCoordinator.swift # Chunked file transfer over IPC (upload + download)
    ├── TransferProgressPanel.swift   # macOS floating progress indicator for file transfers
    ├── Log.swift                     # Simple stderr logger with verbose flag
    └── Generated/                   # Auto-generated Swift protobuf files (do not edit)
        ├── clipboard.pb.swift
        └── clipboard_helper.pb.swift
```

## Key Source Files

- **`ClipboardHelperApp.swift`**: Entry point using `ArgumentParser`. Starts `NSApplication`, installs `AppDelegate`, calls `app.run()` (never returns). `AppDelegate` wires together all subsystems and routes incoming messages.
- **`SocketServer.swift`**: Creates a Unix domain socket, accepts one client connection per instance. Uses `DispatchSourceRead` for async reads. Decodes 4-byte LE length prefix, assembles frames, fires `onMessage`. Socket permissions hardened to `0o600`.
- **`PasteboardManager.swift`**:
  - **Direct set** (`SET_CLIPBOARD`): writes text/image/file promise directly to `NSPasteboard.general`
  - **Delayed rendering** (`ANNOUNCE_DELAYED`): calls `declareTypes:owner:`, waits for OS data request, sends `DATA_REQUEST` to parent, blocks until `PROVIDE_DATA` arrives
  - **Polling** (`CLIPBOARD_CHANGED`): 0.5s timer checks `NSPasteboard.changeCount`, serializes content (text, image as WebP, file URLs as `FileMetadata`)
- **`FileTransferCoordinator.swift`**: Handles chunked file transfers in both directions. Client mode receives via `NSFilePromiseProvider`; server mode serves chunk responses.
- **`TransferProgressPanel.swift`**: Floating progress window driven by `FILE_TRANSFER_PROGRESS` messages from the parent process.

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

When `Proto/*.proto` files change, run `make generate-proto`. This requires `protoc` and the `swift-protobuf` plugin (`protoc-gen-swift`) to be installed. The generated files in `Sources/ClipboardHelper/Generated/` should be committed.

The `.proto` files here are the source of truth — the same definitions are synced into `leviathan/proto/` for Go code generation.
