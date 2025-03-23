set(SOURCES
    BlockAllocator.cpp
    Cell.cpp
    CellAllocator.cpp
    ConservativeVector.cpp
    ForeignCell.cpp
    Root.cpp
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

    # FIXME: https://github.com/swiftlang/swift/issues/80182
    target_compile_options(LibGC PUBLIC $<$<COMPILE_LANGUAGE:C,CXX>:-DLIBGC_WORKAROUND_BOOL_BITFIELD>
                                        SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -DLIBGC_WORKAROUND_BOOL_BITFIELD>)
    add_swift_target_properties(LibGC LAGOM_LIBRARIES AK COMPILE_DEFINITIONS LIBGC_WORKAROUND_BOOL_BITFIELD)
endif()
