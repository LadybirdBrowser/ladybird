set(SOURCES
    Runtime.cpp
)

ladybird_lib(LibWasm wasm EXPLICIT_SYMBOL_EXPORT)

import_rust_crate(MANIFEST_PATH Engine/Cargo.toml CRATE_NAME libwasm_engine FEATURES allocator capi FFI_HEADER RustFFI.h)
target_link_libraries(LibWasm PRIVATE libwasm_engine PUBLIC LibGC)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(LibWasm INTERFACE LINKER:--allow-multiple-definition)
endif()

include(wasm_spec_tests)
