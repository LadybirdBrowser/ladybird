set(SOURCES
    FileSystem.cpp
    TempFile.cpp
)

serenity_lib(LibFileSystem filesystem)
target_link_libraries(LibFileSystem PRIVATE LibCoreMinimal)

if (ENABLE_SWIFT)
    generate_clang_module_map(LibFileSystem
        # GENERATED_FILES
        # "${CMAKE_CURRENT_BINARY_DIR}/Backtrace.h"
    )

    target_sources(LibFileSystem PRIVATE LibFileSystem/LibFileSystem+Swift.swift)
    add_swift_target_properties(LibFileSystem)
endif()
