set(SOURCES
    URL.cpp
    Parser.cpp
)

serenity_lib(LibURL url)
target_link_libraries(LibURL PRIVATE LibUnicode LibTextCodec)
