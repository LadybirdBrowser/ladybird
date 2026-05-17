set(SOURCES
    ECMAScriptRegex.cpp
    RustRegex.cpp
)

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libregex_rust FEATURES allocator FFI_HEADER RustFFI.h)
target_link_libraries(LibRegex PRIVATE libregex_rust LibUnicode)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(LibRegex INTERFACE LINKER:--allow-multiple-definition)
endif()
