ladybird_test_suite(LibIPC)

set(TEST_SOURCES
    TestConnection.cpp
    TestDecoder.cpp
)

if (UNIX AND NOT APPLE)
    list(APPEND TEST_SOURCES TestTransportSocket.cpp)
endif()
if (APPLE)
    list(APPEND TEST_SOURCES TestTransportMachPort.cpp)
    if (NOT IOS)
        list(APPEND TEST_SOURCES TestMachBootstrap.cpp)
    endif()
endif()

if (NOT WIN32)
    compile_ipc(RustEchoServer.ipc RustEchoServerEndpoint.h RUST_OUTPUT RustEchoServerEndpoint.rs)
    set(rust_echo_environment "RUST_ECHO_SERVER_ENDPOINT=${CMAKE_CURRENT_BINARY_DIR}/RustEchoServerEndpoint.rs")
    import_rust_crate(MANIFEST_PATH RustEchoServer/Cargo.toml CRATE_NAME rust_echo_server ENVIRONMENT ${rust_echo_environment})
    add_dependencies(rust_echo_server-build generate_RustEchoServerEndpoint.h)
    test_rust_crate(MANIFEST_PATH RustEchoServer/Cargo.toml CRATE_NAME rust_echo_server ENVIRONMENT ${rust_echo_environment})
    add_dependencies(rust_echo_server-test generate_RustEchoServerEndpoint.h)
    list(APPEND TEST_SOURCES TestRustEchoServer.cpp "${CMAKE_CURRENT_BINARY_DIR}/RustEchoServerEndpoint.cpp")
    target_link_libraries(TestLibIPC PRIVATE rust_echo_server)
    target_include_directories(TestLibIPC PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    add_dependencies(TestLibIPC generate_RustEchoServerEndpoint.h)

    test_rust_crate(MANIFEST_PATH ../../Libraries/LibIPC/Rust/Cargo.toml CRATE_NAME libipc_rust)
endif()

target_sources(TestLibIPC PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibIPC PRIVATE LibIPC LibThreading)
