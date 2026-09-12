add_subdirectory(AK)
add_subdirectory(LibCompress)
add_subdirectory(LibCore)
add_subdirectory(LibCrypto)
add_subdirectory(LibDatabase)
add_subdirectory(LibDiff)
add_subdirectory(LibDNS)
add_subdirectory(LibGC)
add_subdirectory(LibHTTP)
add_subdirectory(LibIPC)
add_subdirectory(LibJS)
add_subdirectory(LibRegex)
add_subdirectory(LibSync)
add_subdirectory(LibTest)
add_subdirectory(LibTextCodec)
add_subdirectory(LibThreading)
add_subdirectory(LibTLS)
add_subdirectory(LibUnicode)
add_subdirectory(LibURL)
add_subdirectory(LibWasm)
add_subdirectory(LibXML)
add_subdirectory(RequestServer)
add_subdirectory(Meta)

# FIXME: Provide standalone implementations of the C++ FFI callbacks required by Rust test binaries on Windows.
if (NOT WIN32)
    set(rust_workspace_test_command
        "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo/build"
            "${RUST_CARGO}" test --workspace
                --manifest-path "${LADYBIRD_SOURCE_DIR}/Cargo.toml"
                --target "${RUST_TARGET_TRIPLE}"
    )
    set(flap_test_command
        "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo/flap-tests"
            "${RUST_CARGO}" test
                --manifest-path "${LADYBIRD_SOURCE_DIR}/Libraries/LibJS/Flap/Cargo.toml"
                --target "${RUST_TARGET_TRIPLE}"
                --all-targets
    )

    set(rust_tests_stamp "${CMAKE_CURRENT_BINARY_DIR}/rust-tests.stamp")
    set(rust_tests_depfile "${CMAKE_CURRENT_BINARY_DIR}/rust-tests.d")
    set(rust_tests_merge_depfiles "${LADYBIRD_SOURCE_DIR}/Meta/Generators/merge-cargo-depfiles.py")

    add_custom_command(
        COMMAND ${rust_workspace_test_command} --no-run --quiet
        COMMAND ${flap_test_command} --no-run --quiet
        COMMAND
            "${Python3_EXECUTABLE}" "${rust_tests_merge_depfiles}"
            --output "${rust_tests_depfile}"
            --stamp "${rust_tests_stamp}"
            --target "${CMAKE_BINARY_DIR}/cargo/build/${RUST_TARGET_TRIPLE}/debug"
            --target "${CMAKE_BINARY_DIR}/cargo/flap-tests/${RUST_TARGET_TRIPLE}/debug"
        COMMAND ${CMAKE_COMMAND} -E touch "${rust_tests_stamp}"
        DEPENDS
            "${rust_tests_merge_depfiles}"
            "${LADYBIRD_SOURCE_DIR}/Cargo.toml"
            "${LADYBIRD_SOURCE_DIR}/Cargo.lock"
            "${LADYBIRD_SOURCE_DIR}/Libraries/LibJS/Flap/Cargo.toml"
            "${LADYBIRD_SOURCE_DIR}/Libraries/LibJS/Flap/Cargo.lock"
            "${LADYBIRD_SOURCE_DIR}/rust-toolchain.toml"
        DEPFILE "${rust_tests_depfile}"
        OUTPUT "${rust_tests_stamp}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Building Rust test binaries"
        JOB_POOL cargo
        COMMAND_EXPAND_LISTS
    )
    add_custom_target(rust-tests ALL DEPENDS "${rust_tests_stamp}")

    add_test(NAME RustWorkspace COMMAND ${rust_workspace_test_command})
    add_test(NAME Flap COMMAND ${flap_test_command})
endif()

if (ENABLE_GUI_TARGETS)
    add_subdirectory(Compositor)
    add_subdirectory(LibDevTools)
    add_subdirectory(LibGfx)
    add_subdirectory(LibMedia)
    add_subdirectory(LibWeb)
    add_subdirectory(LibWebView)
    add_subdirectory(UI)
endif()

if (ENABLE_CLANG_PLUGINS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    add_subdirectory(ClangPlugins)
endif()
