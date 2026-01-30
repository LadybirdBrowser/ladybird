set(SOURCES
    BlockAllocator.cpp
    Cell.cpp
    CellAllocator.cpp
    ConservativeVector.cpp
    Root.cpp
    RootHashMap.cpp
    RootVector.cpp
    Heap.cpp
    HeapBlock.cpp
    WeakBlock.cpp
    WeakContainer.cpp
)

ladybird_lib(LibGC gc EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibGC PRIVATE LibCore)

set_target_properties(LibGC PROPERTIES UNITY_BUILD ${LADYBIRD_ENABLE_UNITY_BUILD})

# TODO: Use lagom_generate_export_header and annotate entire LibGC with export macros
include(GenerateExportHeader)
generate_export_header(LibGC EXPORT_MACRO_NAME GC_API EXPORT_FILE_NAME "Export.h")
