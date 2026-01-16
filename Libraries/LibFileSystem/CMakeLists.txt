set(SOURCES
   FileSystem.cpp
)

ladybird_lib(LibFileSystem filesystem)
target_link_libraries(LibFileSystem PRIVATE LibCore)
