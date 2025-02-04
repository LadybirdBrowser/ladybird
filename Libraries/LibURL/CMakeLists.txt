include(public_suffix)

set(SOURCES
    Host.cpp
    Origin.cpp
    Parser.cpp
    Site.cpp
    URL.cpp
    ${PUBLIC_SUFFIX_SOURCES}
    Pattern/Pattern.cpp
)

serenity_lib(LibURL url)
target_link_libraries(LibURL PRIVATE LibUnicode LibTextCodec LibRegex)
target_compile_definitions(LibURL PRIVATE ENABLE_PUBLIC_SUFFIX=$<BOOL:${ENABLE_PUBLIC_SUFFIX_DOWNLOAD}>)
