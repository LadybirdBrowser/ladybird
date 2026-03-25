if (NOT ENABLE_RUST)
    message(FATAL_ERROR "LibRegex requires ENABLE_RUST; the legacy C++ regex engine has been removed")
endif()

set(SOURCES
    ECMAScriptRegex.cpp
    RustRegex.cpp
)

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libregex_rust)
target_link_libraries(LibRegex PRIVATE libregex_rust)
target_compile_definitions(LibRegex PRIVATE ENABLE_RUST)
