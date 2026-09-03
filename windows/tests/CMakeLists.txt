# Unit tests for clipboard-helper.
#
# We rebuild the small set of production sources we want to exercise as part
# of the test executable rather than depending on an OBJECT library. This
# keeps the test binary free of the OLE / WIC / STA transitive dependencies
# that the production exe pulls in via dispatch.cpp and friends. The codec
# split (dispatch_codec.cpp + string_utils.cpp) is what makes that possible
# for the wire-format surface — neither TU touches OLE.

add_executable(clipboard_helper_tests
    main.cpp
    proto_wire_test.cpp
    session_test.cpp
    pipe_server_test.cpp
    log_test.cpp
    dispatch_codec_test.cpp
    clipboard_ops_unit_test.cpp
    wic_image_test.cpp
    virtual_file_provider_test.cpp
    work_queue_test.cpp
    ${CMAKE_SOURCE_DIR}/src/work_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/proto_wire.cpp
    ${CMAKE_SOURCE_DIR}/src/pipe_server.cpp
    ${CMAKE_SOURCE_DIR}/src/session.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
    ${CMAKE_SOURCE_DIR}/src/dispatch_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_format.cpp
)

target_include_directories(clipboard_helper_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(clipboard_helper_tests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(clipboard_helper_tests PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

# advapi32 — SDDL parsing inside pipe_server (BuildPipeSecurityDescriptor).
# kernel32 is auto-linked.
target_link_libraries(clipboard_helper_tests PRIVATE advapi32)

set_target_properties(clipboard_helper_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard_helper_tests
)

add_test(NAME clipboard_helper_tests
         COMMAND clipboard_helper_tests)

# ──────────────────────────────────────────────────────────────────────
# Paste roundtrip test — separate exe because it pulls OLE / clipboard /
# IDataObject sources that the pure-codec test deliberately excludes.
#
# What it proves: VirtualClipboardDataObject + OleSetClipboard +
# OleGetClipboard + GetData(CFSTR_FILECONTENTS) + IStream::Read all chain
# correctly without Explorer in the loop.  If this passes but Explorer
# paste fails on the same machine, the bug isn't in our IDataObject.
# ──────────────────────────────────────────────────────────────────────
add_executable(clipboard_helper_paste_roundtrip_test
    main.cpp
    paste_roundtrip_test.cpp
    ${CMAKE_SOURCE_DIR}/src/virtual_file_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_format.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/wic_image.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
)

target_include_directories(clipboard_helper_paste_roundtrip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(clipboard_helper_paste_roundtrip_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(clipboard_helper_paste_roundtrip_test PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

# ole32 — OleSetClipboard / OleGetClipboard / OleInitialize.
# oleaut32 — STGMEDIUM helpers (ReleaseStgMedium).
# user32 — RegisterClipboardFormatW.
# shell32 — CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS atoms (auto-resolved
#   via shlobj.h but the .lib is needed for any indirect symbols).
# windowscodecs — WIC image helpers transitively pulled by clipboard_ops.
# advapi32 — RegisterClipboardFormatW transitively + SDDL helpers.
target_link_libraries(clipboard_helper_paste_roundtrip_test PRIVATE
    ole32 oleaut32 user32 shell32 shlwapi uuid
    windowscodecs advapi32
)

set_target_properties(clipboard_helper_paste_roundtrip_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard_helper_paste_roundtrip_test
)

add_test(NAME clipboard_helper_paste_roundtrip_test
         COMMAND clipboard_helper_paste_roundtrip_test)

# ──────────────────────────────────────────────────────────────────────
# WIC image test — PngToBgra / BgraToPng need a live COM apartment, so they
# get their own exe (the pure-codec test exe deliberately excludes WIC /
# windowscodecs). wic_decode_test.cpp ships its own main() that brackets
# RunAll with CoInitializeEx / ResetWicFactoryForThread / CoUninitialize,
# so it does NOT link the shared main.cpp.
# ──────────────────────────────────────────────────────────────────────
add_executable(clipboard_helper_wic_test
    wic_decode_test.cpp
    ${CMAKE_SOURCE_DIR}/src/wic_image.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
)

target_include_directories(clipboard_helper_wic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(clipboard_helper_wic_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(clipboard_helper_wic_test PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

# ole32 — CoCreateInstance(WICImagingFactory). windowscodecs — WIC codecs.
# uuid — CLSID_WICImagingFactory / IID_IWICImagingFactory symbols.
target_link_libraries(clipboard_helper_wic_test PRIVATE
    ole32 windowscodecs uuid
)

set_target_properties(clipboard_helper_wic_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard_helper_wic_test
)

add_test(NAME clipboard_helper_wic_test
         COMMAND clipboard_helper_wic_test)

# ──────────────────────────────────────────────────────────────────────
# virtual_paste_publish — manual Explorer-paste repro tool.  Not a unit
# test (no ctest registration); run by hand, sits in an STA message pump
# holding the OLE clipboard so the operator can paste from Explorer.
# Useful for collecting trace logs of which IDataObject methods Explorer
# actually invokes during a paste attempt.
# ──────────────────────────────────────────────────────────────────────
add_executable(virtual_paste_publish
    virtual_paste_publish.cpp
    ${CMAKE_SOURCE_DIR}/src/virtual_file_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_format.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/wic_image.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
)

target_include_directories(virtual_paste_publish PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(virtual_paste_publish PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(virtual_paste_publish PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

target_link_libraries(virtual_paste_publish PRIVATE
    ole32 oleaut32 user32 shell32 shlwapi uuid
    windowscodecs advapi32
)

set_target_properties(virtual_paste_publish PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME virtual_paste_publish
)

# ──────────────────────────────────────────────────────────────────────
# GoogleTest suites.
#
# The test_lite suites above stay as they are. Suites that want fixtures,
# bounded multi-threaded waits and GTest's assertion vocabulary live in
# clipboard_helper_gtests. GoogleTest is a TEST-ONLY dependency: it is
# fetched at configure time from the pinned release tarball (hash-checked,
# no git clone) and built with the same static CRT as the helper, so the
# production exe still links nothing but Windows system DLLs.
# ──────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(googletest
    URL      https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# googletest's project() enables C as well as CXX. This project only
# configures clang-cl for CXX (build.ps1 passes CMAKE_CXX_COMPILER); left
# alone, CMake picks the GNU-driver clang.exe next to it for C and then
# feeds it MSVC-style flags, and the try-compile fails. Point C at the same
# clang-cl before the language gets enabled.
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH
        "C compiler (mirrors CMAKE_CXX_COMPILER for googletest)" FORCE)
endif()
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Match the helper's /MT[d] runtime (CMAKE_MSVC_RUNTIME_LIBRARY in the root
# CMakeLists). A shared-CRT gtest would not link against our objects.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# chunk_provider.cpp is the FILE_CHUNK_REQUEST / FILE_CHUNK_DATA wait loop
# behind virtual-file paste. It needs only the codec (to encode the
# request) and user32 (MsgWaitForMultipleObjects / PeekMessage) — no OLE,
# no PipeServer — which is what lets it be tested with a captured
# SendFrameFn and a short timeout.
add_executable(clipboard_helper_gtests
    chunk_provider_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chunk_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/dispatch_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/clipboard_format.cpp
    ${CMAKE_SOURCE_DIR}/src/proto_wire.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
)

target_include_directories(clipboard_helper_gtests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(clipboard_helper_gtests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_compile_options(clipboard_helper_gtests PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Debug>:/Od /Zi>
)

# user32 — MsgWaitForMultipleObjects / PeekMessageW / PostThreadMessageW
#   (the wait loop pumps the STA queue; tests drive it the same way).
# advapi32 — transitively via log.cpp / string_utils.
target_link_libraries(clipboard_helper_gtests PRIVATE
    GTest::gtest_main user32 advapi32
)

set_target_properties(clipboard_helper_gtests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard_helper_gtests
)

add_test(NAME clipboard_helper_gtests
         COMMAND clipboard_helper_gtests)
