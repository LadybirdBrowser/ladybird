include(public_suffix)

set(SOURCES
    Host.cpp
    Origin.cpp
    Parser.cpp
    RustIntegration.cpp
    Site.cpp
    URL.cpp
    ${PUBLIC_SUFFIX_SOURCES}
    Pattern/Canonicalization.cpp
    Pattern/Component.cpp
    Pattern/ConstructorStringParser.cpp
    Pattern/Init.cpp
    Pattern/Options.cpp
    Pattern/Part.cpp
    Pattern/Pattern.cpp
    Pattern/PatternParser.cpp
    Pattern/String.cpp
    Pattern/Tokenizer.cpp
)

ladybird_lib(LibURL url)
target_link_libraries(LibURL PRIVATE liburl_rust LibUnicode LibTextCodec LibRegex)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME liburl_rust FEATURES allocator FFI_HEADER RustFFI.h)
target_link_libraries(liburl_rust INTERFACE LibUnicode LibTextCodec LibRegex)
target_compile_definitions(LibURL PRIVATE ENABLE_RUST)
