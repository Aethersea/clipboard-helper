cmake_minimum_required(VERSION 3.21)

project(clipboard-helper-linux
    VERSION 0.1.0
    # CXX for our own code; C for the wayland-scanner-generated
    # protocol stubs (-protocol.c). Without C in the language list
    # CMake silently SKIPS .c sources passed to add_executable —
    # producing an empty link with undefined references to every
    # `*_interface` descriptor.
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable C++20 module scanning; we don't use modules and scanning emits a
# malformed `unscanned_$Config` rule with CMake 4.x + Ninja single-config.
# (Same workaround the Windows helper uses.)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "clipboard-helper-linux targets Linux; use macos/ or windows/ for other platforms")
endif()

# Auto-run the meta-object compiler on Q_OBJECT-bearing headers. We use
# QObject + Qt signals/slots in wayland_display.* and friends; without
# AUTOMOC the generated _moc files never compile.
set(CMAKE_AUTOMOC ON)

# ─── Qt 6 ──────────────────────────────────────────────────────────────────
#
# Core is always required (QCoreApplication event loop, QSocketNotifier for
# the wl_display fd and the signal self-pipe).
#
# Gui is required only when the X11 backend is reachable — QClipboard +
# QMimeData::retrieveData live in QtGui and need a QGuiApplication. The
# Wayland backend talks libwayland-client directly and stays QtCore-only
# (no QPA plugin → no risk of double-grabbing the wl_display socket).
#
# Floor: Qt 6.4 — matches what Debian 13 (trixie, 6.4.2) and Ubuntu
# 24.04 ship by default, so distro-package installs suffice without
# pulling in aqtinstall / Qt installer. Nothing in our actual usage
# (QCoreApplication, QGuiApplication, QClipboard, QMimeData,
# QImage{Reader,Writer}) is 6.5-specific.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui)

# ─── Wayland (libwayland-client + wayland-scanner) ─────────────────────────
#
# The Wayland backend speaks `wlr-data-control-unstable-v1` directly. The
# protocol XML is fetched into protocols/ via fetch-protocols.sh (one-time
# dev setup); we run wayland-scanner over it at build time to generate the
# C client stubs.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

# ─── libwebp (WebP image decode) ───────────────────────────────────────────
#
# The clipboard IMAGE wire format is lossless WebP. We decode it via libwebp
# directly (see src/webp_decode.*) rather than the optional Qt image-format
# plugin (qt6-image-formats-plugins / qt6-qtimageformats), which is trivially
# missing on a minimal install and silently breaks image paste. PNG/BMP
# re-encoding still uses qtbase's built-in image writers (no plugin needed).
pkg_check_modules(LIBWEBP REQUIRED IMPORTED_TARGET libwebp)

find_program(WAYLAND_SCANNER_EXECUTABLE
    NAMES wayland-scanner
    DOC "wayland-scanner code generator (from libwayland-dev / wayland-devel)")
if(NOT WAYLAND_SCANNER_EXECUTABLE)
    message(FATAL_ERROR
        "wayland-scanner not found; install libwayland-dev (Debian/Ubuntu) "
        "or wayland-devel (Fedora) and rerun cmake configure.")
endif()

set(WAYLAND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-generated")
file(MAKE_DIRECTORY "${WAYLAND_GENERATED_DIR}")

# wayland_protocol_codegen(<basename> <output-var-c> <output-var-h>)
# Runs wayland-scanner on protocols/<basename>.xml to produce
# <basename>-protocol.c and <basename>-client-protocol.h, then sets
# the named variables in the calling scope. Fails configure if the
# XML is missing (caller should ./protocols/fetch-protocols.sh first).
function(wayland_protocol_codegen basename out_c out_h)
    set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${basename}.xml")
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR
            "Wayland protocol XML missing at ${xml}. "
            "Run ./protocols/fetch-protocols.sh first to download it "
            "from upstream (one-time setup; see protocols/README.md).")
    endif()
    set(header "${WAYLAND_GENERATED_DIR}/${basename}-client-protocol.h")
    set(code   "${WAYLAND_GENERATED_DIR}/${basename}-protocol.c")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${xml}" "${header}"
        DEPENDS "${xml}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${code}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${xml}" "${code}"
        DEPENDS "${xml}"
        VERBATIM)
    set("${out_c}" "${code}"   PARENT_SCOPE)
    set("${out_h}" "${header}" PARENT_SCOPE)
endfunction()

wayland_protocol_codegen(wlr-data-control-unstable-v1 WLR_PRIVATE_CODE WLR_CLIENT_HEADER)
wayland_protocol_codegen(ext-data-control-v1          EXT_PRIVATE_CODE EXT_CLIENT_HEADER)

add_custom_target(wayland_protocol_codegen_all
    DEPENDS "${WLR_CLIENT_HEADER}" "${WLR_PRIVATE_CODE}"
            "${EXT_CLIENT_HEADER}" "${EXT_PRIVATE_CODE}")

# ─── Helper executable ─────────────────────────────────────────────────────
#
# Intentionally NO libprotobuf dependency, matching the Windows helper.
# proto_wire.* hand-rolls varint + length-delim, which is enough for every
# HelperMessage field we care about. Keeps the helper free of a heavyweight
# C++ runtime dependency that distros ship in inconsistent versions.

add_executable(clipboard-helper
    src/main.cpp
    src/log.cpp
    src/proto_wire.cpp
    src/dispatch.cpp
    src/dispatch_codec.cpp
    src/socket_server.cpp
    src/backend_detector.cpp
    src/parent_watchdog.cpp
    src/clipboard_manager.cpp
    src/clipboard_manager_wayland.cpp
    src/clipboard_manager_ext.cpp
    src/clipboard_manager_x11.cpp
    src/wayland_display.cpp
    src/wayland_probe.cpp
    src/uri_list_format.cpp
    src/webp_decode.cpp
    src/echo_suppressor.cpp
    "${WLR_PRIVATE_CODE}"
    "${EXT_PRIVATE_CODE}"
)

add_dependencies(clipboard-helper wayland_protocol_codegen_all)

target_include_directories(clipboard-helper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${WAYLAND_GENERATED_DIR}
)

target_compile_options(clipboard-helper PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-O0 -g>
)

# The wayland-scanner-generated C code uses K&R-style casts that trip
# -Wpedantic; isolate the relaxation to those TUs instead of relaxing
# the whole build.
set_source_files_properties("${WLR_PRIVATE_CODE}" "${EXT_PRIVATE_CODE}" PROPERTIES
    COMPILE_FLAGS "-Wno-pedantic -Wno-error")

target_link_libraries(clipboard-helper PRIVATE
    Qt6::Core
    Qt6::Gui
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::LIBWEBP
    pthread
)

set_target_properties(clipboard-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard-helper
)

# ─── Tests ─────────────────────────────────────────────────────────────────
option(CH_BUILD_TESTS "Build clipboard-helper unit tests" ON)
if(CH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
