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
