set(SOURCES
    AbstractMachine/AbstractMachine.cpp
    AbstractMachine/BytecodeInterpreter.cpp
    AbstractMachine/Configuration.cpp
    AbstractMachine/Validator.cpp
    Parser/Parser.cpp
    Printer/Printer.cpp
)

# FIXME: Add Windows support
if (NOT WIN32)
    list(APPEND SOURCES WASI/Wasi.cpp)
endif()

option(ENABLE_CRANELIFT_JIT "Enable Cranelift-based AOT compilation for WebAssembly" OFF)

set(WASM_COMPILED_FAULT_RECOVERY_SUPPORTED 0)

if(WIN32 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|ARM64|arm64)$"))
    set(WASM_COMPILED_FAULT_RECOVERY_SUPPORTED 1)
elseif(APPLE)
    set(WASM_COMPILED_FAULT_RECOVERY_SUPPORTED 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64)$"))
    set(WASM_COMPILED_FAULT_RECOVERY_SUPPORTED 1)
endif()

if (ENABLE_CRANELIFT_JIT)
    list(APPEND SOURCES CraneliftBridge.cpp)
    build_rust_binary(
        MANIFEST_PATH Rust/Cargo.toml
        CRATE_NAME libwasm_cranelift
        BINARY_NAME cranelift-compiler
        OUTPUT_PATH_VAR WASM_CRANELIFT_COMPILER_BINARY
        FFI_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
    )
else()
    list(APPEND SOURCES CraneliftStubs.cpp)
endif()

ladybird_lib(LibWasm wasm EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWasm PRIVATE LibCore)

target_compile_definitions(LibWasm PRIVATE
    WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=${WASM_COMPILED_FAULT_RECOVERY_SUPPORTED}
)

if (ENABLE_CRANELIFT_JIT)
    add_dependencies(LibWasm cranelift-compiler-build)
    target_include_directories(LibWasm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(LibWasm PRIVATE
        WASM_CRANELIFT=1
        WASM_CRANELIFT_COMPILER_PATH="${WASM_CRANELIFT_COMPILER_BINARY}"
    )
endif()

include(wasm_spec_tests)
