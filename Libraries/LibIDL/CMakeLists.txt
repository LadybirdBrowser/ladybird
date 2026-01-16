set(SOURCES
    ExposedTo.cpp
    IDLParser.cpp
    Types.cpp
)

ladybird_lib(LibIDL idl)
target_link_libraries(LibIDL PRIVATE LibCore LibFileSystem)
