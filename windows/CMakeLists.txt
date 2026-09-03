cmake_minimum_required(VERSION 3.20)

project(leviathan-clipboard-helper
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# We don't use C++20 modules. The CMake 4.3 + Ninja single-config combo emits
# a malformed `unscanned_$Config` rule name when module scanning is implicitly
# enabled. Disable scanning to keep rule generation legible to ninja.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT MSVC)
    message(FATAL_ERROR "leviathan-clipboard-helper requires clang-cl or MSVC (got compiler: ${CMAKE_CXX_COMPILER_ID})")
endif()

# Statically link the MSVC C++ / UCRT runtime (/MT, /MTd for Debug).
#
# The helper otherwise imports only Windows system DLLs (ole32, user32,
# windowscodecs, ...), so a static CRT makes the exe fully self-contained:
# leviathan ships it next to leviathan.exe and it runs on any Windows 10/11
# host WITHOUT requiring the "Visual C++ 2015-2022 Redistributable"
# (msvcp140.dll / vcruntime140*.dll / api-ms-win-crt-*). The +~150 KB of
# inlined CRT is negligible against avoiding that deployment dependency.
#
# Requires CMP0091=NEW (default at cmake_minimum_required 3.20) so this knob
# drives the runtime selection instead of the legacy /MD flags. clang-cl
# honours MSVC_RUNTIME_LIBRARY and emits the matching /MT[d].
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─── Helper executable ─────────────────────────────────────────────────────
#
# We deliberately do NOT depend on libprotobuf. The HelperMessage schema has a
# small enough surface (~12 message types, varint + length-delim only) that
# hand-rolled wire-format primitives (src/proto_wire.*) keep the helper free
# of external C++ runtime dependencies and vcvarsall.bat / vcpkg toolchain
# headaches.

add_executable(leviathan-clipboard-helper
    src/main.cpp
    src/pipe_server.cpp
    src/log.cpp
    src/session.cpp
    src/proto_wire.cpp
    src/dispatch.cpp
    src/chunk_provider.cpp
    src/dispatch_codec.cpp
    src/sta_worker.cpp
    src/work_queue.cpp
    src/clipboard_ops.cpp
    src/clipboard_format.cpp
    src/string_utils.cpp
    src/virtual_file_provider.cpp
    src/wic_image.cpp
)

target_include_directories(leviathan-clipboard-helper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(leviathan-clipboard-helper PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(leviathan-clipboard-helper PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

target_link_libraries(leviathan-clipboard-helper PRIVATE
    ole32
    oleaut32
    user32
    advapi32
    wtsapi32
    shell32
    shlwapi
    uuid
    windowscodecs   # WIC: image decode/encode (PNG <-> BGRA <-> DIB)
)

set_target_properties(leviathan-clipboard-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME leviathan-clipboard-helper
)

# ─── Tests ─────────────────────────────────────────────────────────────────
option(LH_BUILD_TESTS "Build clipboard-helper unit tests" ON)
if(LH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
