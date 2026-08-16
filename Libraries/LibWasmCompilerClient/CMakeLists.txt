set(SOURCES
    Client.cpp
    State.cpp
    ThreadedClient.cpp
)

compile_ipc(../../Services/WasmCompiler/WasmCompilerClient.ipc ../../Services/WasmCompiler/WasmCompilerClientEndpoint.h)
compile_ipc(../../Services/WasmCompiler/WasmCompilerServer.ipc ../../Services/WasmCompiler/WasmCompilerServerEndpoint.h)

set(GENERATED_SOURCES
    ../../Services/WasmCompiler/WasmCompilerClientEndpoint.h
    ../../Services/WasmCompiler/WasmCompilerServerEndpoint.h
)

ladybird_lib(LibWasmCompilerClient wasmcompilerclient)
target_link_libraries(LibWasmCompilerClient PRIVATE LibCore LibIPC LibSync LibThreading LibWasm)
