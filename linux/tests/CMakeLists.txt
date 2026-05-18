# Unit tests for clipboard-helper (Linux).
#
# Like the Windows helper's tests, we rebuild the small set of
# production sources we want to exercise as part of the test
# executable rather than depending on an OBJECT library. That keeps
# the test binary free of any Qt6 transitive dependencies (which
# the production main() pulls in via QCoreApplication) — every test
# here runs in pure POSIX with no display required.

add_executable(clipboard_helper_tests
    main.cpp
    proto_wire_test.cpp
    backend_detector_test.cpp
    log_test.cpp
    dispatch_codec_test.cpp
    socket_server_test.cpp
    parent_watchdog_test.cpp
    uri_list_format_test.cpp
    ${CMAKE_SOURCE_DIR}/src/proto_wire.cpp
    ${CMAKE_SOURCE_DIR}/src/backend_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
    ${CMAKE_SOURCE_DIR}/src/dispatch_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/socket_server.cpp
    ${CMAKE_SOURCE_DIR}/src/parent_watchdog.cpp
    ${CMAKE_SOURCE_DIR}/src/uri_list_format.cpp
)

target_include_directories(clipboard_helper_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(clipboard_helper_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-O0 -g>
)

target_link_libraries(clipboard_helper_tests PRIVATE
    pthread
)

set_target_properties(clipboard_helper_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME clipboard_helper_tests
)

add_test(NAME clipboard_helper_tests
         COMMAND clipboard_helper_tests)

# Modest timeout: socket_server tests do up to ~2s per case for
# connect / EOF waits; the suite has ~10 such cases.
set_tests_properties(clipboard_helper_tests PROPERTIES TIMEOUT 60)
