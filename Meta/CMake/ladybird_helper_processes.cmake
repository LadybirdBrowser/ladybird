set(ladybird_helper_processes
    Compositor
    ImageDecoder
    RequestServer
    WebContent
    WebWorker
)

if (ENABLE_CRANELIFT_JIT)
    list(APPEND ladybird_helper_processes WasmCompiler)
endif()
