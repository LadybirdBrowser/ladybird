set(SOURCES
    BlockAllocator.cpp
    Cell.cpp
    CellAllocator.cpp
    ConservativeVector.cpp
    ForeignCell.cpp
    Root.cpp
    RootHashMap.cpp
    RootVector.cpp
    Heap.cpp
    HeapBlock.cpp
    WeakContainer.cpp
)

serenity_lib(LibGC gc)
target_link_libraries(LibGC PRIVATE LibCore)

if (ENABLE_SWIFT)
    generate_clang_module_map(LibGC)
    target_sources(LibGC PRIVATE
        Heap+Swift.swift
    )
    target_link_libraries(LibGC PRIVATE AK)
    add_swift_target_properties(LibGC LAGOM_LIBRARIES AK)
endif()

# TODO: Use lagom_generate_export_header and annotate entire LibGC with export macros
include(GenerateExportHeader)
generate_export_header(LibGC EXPORT_MACRO_NAME GC_API EXPORT_FILE_NAME "Export.h")
