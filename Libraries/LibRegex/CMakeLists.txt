set(SOURCES
    ECMAScriptRegex.cpp
    RustRegex.cpp
)

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)

import_rust_ffi_crate(LibRegex MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libregex_rust_core FFI_HEADER RustFFI.h)
