set(SOURCES
    Origin.cpp
    Parser.cpp
    URL.cpp
)

serenity_lib(LibURL url)
target_link_libraries(LibURL PRIVATE LibUnicode LibTextCodec)
