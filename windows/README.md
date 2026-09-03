# Windows clipboard-helper

See also: [macOS sibling](../macos/) and [root README](../README.md).  
Shared protobuf schema: [../Proto/clipboard_helper.proto](../Proto/clipboard_helper.proto)

A user-mode clipboard helper for Windows that manages OLE/COM clipboard operations in the interactive session. This project is the Windows component of the consolidated `clipboard-helper` repository, solving cross-session OLE failures and supporting advanced clipboard features.

## Status

**Phase 1 skeleton complete**: Named pipe communication with length-prefix framing is implemented and verified. OLE integration and Protobuf message dispatching are planned for subsequent phases.

## Prerequisites

- **Visual Studio Build Tools 2022+** (or BuildTools 2026) with the `Microsoft.VisualStudio.Workload.VCTools` workload.
- **clang-cl** (LLVM/Clang 16+, e.g., `scoop install llvm`).
- **CMake** 3.20+.
- **Ninja** 1.10+.
- **Windows SDK** 10.0.19041+.

## Build

Execute the provided build script to compile the project:

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Config Release
```

The `build.ps1` script automatically discovers the Visual Studio installation root, selects the newest complete MSVC toolset under `VC\Tools\MSVC`, and configures the environment (INCLUDE, LIB, PATH) before invoking CMake and Ninja.

## Tests

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Config Release -RunTests
```

`-RunTests` runs every ctest target after the build (CI does the same with `ctest --test-dir build --build-config Release --output-on-failure`):

| Target | Framework | What it covers |
|---|---|---|
| `clipboard_helper_tests` | `tests/test_lite.h` (header-only) | wire codec, session, pipe server, log, work queue, descriptor builder |
| `clipboard_helper_gtests` | GoogleTest | `DispatcherChunkProvider` — the FILE_CHUNK_REQUEST / FILE_CHUNK_DATA wait loop behind virtual-file paste (timeout, correlation, cancel, message pumping) |
| `clipboard_helper_paste_roundtrip_test` | test_lite | `IDataObject` server through a real OLE clipboard round trip |
| `clipboard_helper_wic_test` | test_lite | WIC PNG ↔ BGRA |

GoogleTest is a test-only dependency: `tests/CMakeLists.txt` fetches the pinned release tarball (SHA-256 checked) at configure time, so the first configure needs network access; the production exe still links only Windows system DLLs. New suites that want fixtures or bounded multi-threaded waits go into `clipboard_helper_gtests`; the test_lite suites stay as they are.

Two local gotchas: `build.ps1` sets both `CMAKE_CXX_COMPILER` and `CMAKE_C_COMPILER` to clang-cl because GoogleTest's `project()` enables C and CMake would otherwise pick the GNU-driver `clang.exe` and feed it MSVC flags. And `clipboard_helper_paste_roundtrip_test` takes the real OLE clipboard, so it fails intermittently while a live `leviathan-clipboard-helper.exe` (e.g. one spawned by a local leviathan) is watching the clipboard — stop that helper or trust the CI run for that target.

## Run (Smoke Test)

You can launch the helper manually or via a parent process. To test the pipe connection from PowerShell:

1. Start the helper: `.\build\Release\leviathan-clipboard-helper.exe`
2. Connect from another PowerShell session:
   ```pwsh
   $sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
   $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(".", "leviathan-clipboard-$sessionId", [System.IO.Pipes.PipeDirection]::InOut)
   $pipe.Connect(5000)
   
   # Send length-prefixed "hello" (0x05 0x00 0x00 0x00 + payload)
   $payload = [System.Text.Encoding]::UTF8.GetBytes("hello")
   $len = [BitConverter]::GetBytes([uint32]$payload.Length)
   $pipe.Write($len, 0, 4)
   $pipe.Write($payload, 0, $payload.Length)
   
   # Read echo response (prefixed with 0x01 in Phase 1)
   $resLenBuf = New-Object byte[] 4
   $pipe.Read($resLenBuf, 0, 4)
   $resLen = [BitConverter]::ToUInt32($resLenBuf, 0)
   $resPayload = New-Object byte[] $resLen
   $pipe.Read($resPayload, 0, $resLen)
   [System.Text.Encoding]::UTF8.GetString($resPayload)
   ```

## Architecture

- **Main Thread**: Runs the named pipe accept loop and serves clients sequentially.
- **Watchdog Thread**: Monitors the parent process PID (passed via `--parent-pid`); automatically stops the helper if the parent exits.
- **Future**: STA worker threads for OLE operations (Phase 3+) and Protobuf-based message dispatching (Phase 2).

## IPC Protocol

- **Pipe Name**: `\\.\pipe\leviathan-clipboard-{sessionId}`
- **Frame Format**: `uint32 LE length || payload bytes`
- **Phase 1 Payload**: Opaque bytes; the echo handler prepends `0x01` to the response.
- **Phase 2+ Payload**: `HelperMessage` Protobuf, reusing the shared schema at [../Proto/clipboard_helper.proto](../Proto/clipboard_helper.proto).
- **Security**: SDDL ACL denies Network Users, allows SYSTEM (R/W), and allows the owner (R/W/X).

## Roadmap

1. **Phase 1 (Done)**: CMake build system, named pipe server, and length-prefix framing.
2. **Phase 2**: Protobuf-lite integration; Poll/Apply for text and images.
3. **Phase 3**: Support for `CF_HDROP` file paths.
4. **Phase 4**: `CFSTR_FILEDESCRIPTORW` and `IDataObject` server (delayed rendering).
5. **Phase 5**: Error recovery and removal of legacy OLE Go code from Leviathan.

## Related

- macOS counterpart: [../macos/](../macos/)
- Parent: [aethersea/leviathan](https://github.com/aethersea/leviathan)
- Client: [aethersea/shen](https://github.com/aethersea/shen)
