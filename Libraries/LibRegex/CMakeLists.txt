set(SOURCES
    ECMAScriptRegex.cpp
    RustRegex.cpp
)

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libregex_rust FFI_HEADER RustFFI.h)
target_link_libraries(LibRegex PRIVATE libregex_rust)
