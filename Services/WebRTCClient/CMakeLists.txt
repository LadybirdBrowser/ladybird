import_rust_crate(MANIFEST_PATH Cargo.toml CRATE_NAME webrtcclient_service FEATURES allocator
    ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
add_dependencies(webrtcclient_service-build LibWebRTCClient)

add_executable(WebRTCClient main.cpp)

if (LINUX)
    target_sources(WebRTCClient PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(WebRTCClient PRIVATE SandboxMacOS.cpp)
else()
    target_sources(WebRTCClient PRIVATE SandboxUnimplemented.cpp)
endif()

target_include_directories(WebRTCClient PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(WebRTCClient PRIVATE LibCore LibMain LibSandbox webrtcclient_service)

if (WIN32)
    target_link_libraries(WebRTCClient PRIVATE LibIPC bcrypt advapi32 iphlpapi dbghelp)
    ladybird_windows_bin(WebRTCClient CONSOLE)
endif()

ladybird_sign_helper_process(WebRTCClient)
