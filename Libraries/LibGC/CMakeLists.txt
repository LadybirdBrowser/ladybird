set(SOURCES
    BlockAllocator.cpp
    Cell.cpp
    CellAllocator.cpp
    ConservativeVector.cpp
    ForeignCell.cpp
    Root.cpp
    Heap.cpp
    HeapBlock.cpp
    MarkedVector.cpp
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
