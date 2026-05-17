# clipboard-helper

Multi-platform helper processes that manage system clipboards (macOS `NSPasteboard` and Windows OLE), communicating with parent processes via length-prefixed protobuf messages.

This repository provides separate implementations for macOS and Windows, as both platforms require an active event loop and specific API handling to support advanced features like delayed rendering (lazy clipboard transfer).

## Platforms

- **macos/**: Swift + AppKit implementation for macOS 13+. Uses an `NSApplication` run loop to handle `declareTypes:owner:` callbacks.
- **windows/**: C++17 + clang-cl implementation for Windows 10/11. Manages OLE/COM clipboard operations in the interactive session.
- **linux/**: C++17 + Qt 6 implementation. Runtime-detects backend (Wayland `ext-data-control-v1` / `wlr-data-control-unstable-v1`, X11 via Qt `xcb` QClipboard, GNOME forced to XWayland). **P1 skeleton landed** — IPC + dispatch + parent watchdog + tests; Wayland/X11 backends stubbed pending next PR.

## Shared Wire Protocol

Both implementations speak the same length-prefixed `HelperMessage` protobuf protocol over a local socket (Unix domain socket on macOS, Named Pipe on Windows).

- **Wire format**: 4-byte little-endian length prefix + serialized protobuf.
- **Schema**: [Proto/clipboard_helper.proto](Proto/clipboard_helper.proto).
- **Transport**: macOS / Linux use AF_UNIX (`SOCK_STREAM`, mode 0600), Windows uses a Named Pipe.

## Architecture

```
┌──────────────────┐      IPC        ┌────────────────────────────┐
│  Go server       │ ◄──────────────► │  clipboard-helper          │
│  (leviathan)     │   protobuf IPC  │  - Event loop / Run loop   │
└──────────────────┘                  │  - OS Clipboard owner      │
                                      │  - Delayed rendering       │
┌──────────────────┐      IPC        │  - Clipboard polling       │
│  Electron client │ ◄──────────────► │                            │
│  (Rust + Node)   │   protobuf IPC  └────────────────────────────┘
└──────────────────┘
```

## Building

Refer to platform-specific instructions:
- [macOS Building](macos/Makefile)
- [Windows Building](windows/README.md)
- [Linux Building](linux/README.md) (Qt 6 + CMake; `cd linux && ./build.sh`)

## Usage

### macOS
```bash
macos/ClipboardHelper --socket /tmp/clipboard-helper.sock [--mode server|client] [--verbose]
```

### Windows
```bash
windows/leviathan-clipboard-helper.exe --parent-pid <pid>
```

### Linux
```bash
linux/build/clipboard-helper --socket /tmp/clipboard-helper.sock [--mode server|client] [--parent-pid <pid>] [--verbose]
```

## Message Flow

### Setting clipboard directly
```
Parent → SET_CLIPBOARD(ClipboardData) → Helper
Helper writes data to OS Clipboard
```

### Delayed rendering (lazy transfer)
```
Parent → ANNOUNCE_DELAYED(ClipboardAnnouncement) → Helper
Helper announces availability to OS

[User pastes in another app]

Helper → DATA_REQUEST(ClipboardDataRequest) → Parent
Parent → PROVIDE_DATA(HelperProvideData) → Helper
Helper provides data to requesting app
```

### Local clipboard change detection
```
[User copies in another app]

Helper detects change via polling or OS events
Helper → CLIPBOARD_CHANGED(ClipboardData) → Parent
```

## Related Projects

- [leviathan](https://github.com/aethersea/leviathan) — Go server
- [shen](https://github.com/aethersea/shen) — Electron client
