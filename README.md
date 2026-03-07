# clipboard-helper

A macOS helper process that manages `NSPasteboard` with a proper `NSApplication` run loop, communicating with parent processes via Unix domain socket + length-prefixed protobuf messages.

## Problem

On macOS, `declareTypes:owner:` (delayed/lazy clipboard rendering) requires an active `NSApplication` run loop. Without it, Universal Clipboard / Handoff immediately steals pasteboard ownership, causing the delayed rendering callback to never fire.

This helper solves the problem by running as a separate process with its own `NSApplication` event loop, acting as the pasteboard owner on behalf of the parent process.

## Architecture

```
┌──────────────────┐   Unix Socket   ┌────────────────────────────┐
│  Go server       │ ◄──────────────► │  clipboard-helper (Swift)  │
│  (leviathan)     │   protobuf IPC  │  - NSApplication run loop  │
└──────────────────┘                  │  - NSPasteboard owner      │
                                      │  - declareTypes:owner:     │
┌──────────────────┐   Unix Socket   │  - pasteboard polling      │
│  Electron client │ ◄──────────────► │                            │
│  (Rust + Node)   │   protobuf IPC  └────────────────────────────┘
└──────────────────┘
```

## IPC Protocol

Wire format: 4-byte little-endian length prefix + serialized protobuf (`HelperMessage`).

See [Proto/clipboard_helper.proto](Proto/clipboard_helper.proto) for message definitions.

## Building

```bash
# Debug build
make build

# Release build (current architecture)
make build-release

# Universal binary (arm64 + x86_64)
make build-universal

# Regenerate protobuf Swift sources (requires protoc + swift-protobuf)
make generate-proto
```

## Usage

```bash
clipboard-helper --socket /tmp/clipboard-helper.sock [--mode server|client] [--verbose]
```

### Options

- `--socket <path>` — Path to the Unix domain socket (required)
- `--mode <server|client>` — Operating mode (default: `server`)
  - `server`: Connected to Go server — polls local pasteboard, reports changes
  - `client`: Connected to Electron client — receives remote content
- `--verbose` — Enable debug logging to stderr

## Message Flow

### Setting clipboard directly
```
Parent → SET_CLIPBOARD(ClipboardData) → Helper
Helper writes data to NSPasteboard
```

### Delayed rendering (lazy transfer)
```
Parent → ANNOUNCE_DELAYED(ClipboardAnnouncement) → Helper
Helper calls declareTypes:owner: on NSPasteboard

[User pastes in another app]

Helper → DATA_REQUEST(ClipboardDataRequest) → Parent
Parent → PROVIDE_DATA(HelperProvideData) → Helper
Helper provides data to NSPasteboard (pasteboard:provideDataForType:)
```

### Local clipboard change detection
```
[User copies in another app]

Helper detects change via polling
Helper → CLIPBOARD_CHANGED(ClipboardData) → Parent
```

## Dependencies

- [apple/swift-protobuf](https://github.com/apple/swift-protobuf) — Protocol Buffers for Swift
- [apple/swift-argument-parser](https://github.com/apple/swift-argument-parser) — CLI argument parsing
- macOS 13+ (Ventura)
