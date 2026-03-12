set(SOURCES
    Document.cpp
    Highlighter.cpp
    Language.cpp
)

ladybird_lib(LibSyntax syntax)
target_link_libraries(LibSyntax PRIVATE LibGfx)
