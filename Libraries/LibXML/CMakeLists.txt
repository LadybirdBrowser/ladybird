set(SOURCES
    Parser/Parser.cpp
    DOM/Node.cpp
    XPath/Program.cpp
)

ladybird_lib(LibXML xml EXPLICIT_SYMBOL_EXPORT)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libxml_rust FEATURES allocator FFI_HEADER RustFFI.h)
target_link_libraries(LibXML PRIVATE libxml_rust)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(LibXML INTERFACE LINKER:--allow-multiple-definition)
endif()
