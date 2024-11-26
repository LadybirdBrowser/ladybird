include(public_suffix)

set(SOURCES
    Origin.cpp
    Parser.cpp
    URL.cpp
    ${PUBLIC_SUFFIX_SOURCES}
)

serenity_lib(LibURL url)
target_link_libraries(LibURL PRIVATE LibUnicode LibTextCodec)
target_compile_definitions(LibURL PRIVATE ENABLE_PUBLIC_SUFFIX=$<BOOL:${ENABLE_PUBLIC_SUFFIX_DOWNLOAD}>)
