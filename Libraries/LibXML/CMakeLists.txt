set(SOURCES
    Parser/Parser.cpp
    DOM/Node.cpp
)

ladybird_lib(LibXML xml EXPLICIT_SYMBOL_EXPORT)

target_link_libraries(LibXML PRIVATE LibXml2::LibXml2)
