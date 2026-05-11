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
    src/sta_worker.cpp
    src/clipboard_ops.cpp
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
