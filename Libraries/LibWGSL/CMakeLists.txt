set(SOURCES
    Lexer.cpp
    Preprocessor.cpp
)

ladybird_lib(LibWGSL wgsl EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWGSL PRIVATE AK)
