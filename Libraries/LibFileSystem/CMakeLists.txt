set (SOURCES FileSystem.cpp)
if (NOT WIN32)
   list(APPEND SOURCES TempFile.cpp)
endif()

serenity_lib(LibFileSystem filesystem)
target_link_libraries(LibFileSystem PRIVATE LibCoreMinimal)

if (WIN32)
    find_path(DIRENT_INCLUDE_DIR dirent.h REQUIRED)
    target_include_directories(LibFileSystem PRIVATE ${DIRENT_INCLUDE_DIR})
endif()
