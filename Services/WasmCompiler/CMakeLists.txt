set(SOURCES
    Compiler.cpp
    ConnectionFromClient.cpp
)

add_library(wasmcompilerservice STATIC ${SOURCES})
add_executable(WasmCompiler main.cpp)

if (LINUX)
    target_sources(WasmCompiler PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(WasmCompiler PRIVATE SandboxMacOS.cpp)
else()
    target_sources(WasmCompiler PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(wasmcompilerservice
    generate_WasmCompilerClientEndpoint.h
    generate_WasmCompilerServerEndpoint.h
)

target_include_directories(wasmcompilerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(wasmcompilerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(WasmCompiler PRIVATE wasmcompilerservice LibCore LibMain LibSandbox LibThreading)
target_link_libraries(wasmcompilerservice PRIVATE LibCore LibCrypto LibIPC LibSync LibThreading LibWasm)

if (WIN32)
    ladybird_windows_bin(WasmCompiler CONSOLE)
endif()
