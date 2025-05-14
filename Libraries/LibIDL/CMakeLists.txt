set(SOURCES
    IDLParser.cpp
    Types.cpp
)

serenity_lib(LibIDL idl ENABLE_WINDOWS)
target_link_libraries(LibIDL PRIVATE LibCoreMinimal LibFileSystem)
