set(SOURCES
    RegexByteCode.cpp
    RegexLexer.cpp
    RegexMatcher.cpp
    RegexOptimizer.cpp
    RegexParser.cpp
)

if(SERENITYOS)
    list(APPEND SOURCES C/Regex.cpp)
endif()

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)

if (ENABLE_RUST)
    target_sources(LibRegex PRIVATE RustRegex.cpp)
    import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libregex_rust)
    target_link_libraries(LibRegex PRIVATE libregex_rust)
    target_compile_definitions(LibRegex PRIVATE ENABLE_RUST)
endif()
