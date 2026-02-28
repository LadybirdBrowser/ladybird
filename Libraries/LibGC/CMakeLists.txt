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

find_package(cpptrace CONFIG)
if(cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
    target_link_libraries(LibGC PRIVATE cpptrace::cpptrace)
    target_compile_definitions(LibGC PRIVATE LIBGC_HAS_CPPTRACE=1)
endif()

# TODO: Use lagom_generate_export_header and annotate entire LibGC with export macros
include(GenerateExportHeader)
generate_export_header(LibGC EXPORT_MACRO_NAME GC_API EXPORT_FILE_NAME "Export.h")
