set(SOURCES
    FileSystem.cpp
    TempFile.cpp
)

serenity_lib(LibFileSystem filesystem)
target_link_libraries(LibFileSystem PRIVATE LibCoreMinimal)
