include(public_suffix)

set(SOURCES
    Host.cpp
    Origin.cpp
    Parser.cpp
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
target_link_libraries(LibURL PRIVATE LibUnicode LibTextCodec LibRegex)
