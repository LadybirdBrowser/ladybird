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
