set(SOURCES
    Parser/Parser.cpp
    DOM/Node.cpp
)

ladybird_lib(LibXML xml EXPLICIT_SYMBOL_EXPORT)

find_package(LibXml2 REQUIRED)
target_link_libraries(LibXML PRIVATE LibXml2::LibXml2)
