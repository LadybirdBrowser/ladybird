set (SOURCES FileSystem.cpp)
if (NOT WIN32)
   list(APPEND SOURCES TempFile.cpp)
endif()

serenity_lib(LibFileSystem filesystem)
target_link_libraries(LibFileSystem PRIVATE LibCoreMinimal)
