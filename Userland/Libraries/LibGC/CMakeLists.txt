set(SOURCES
    BlockAllocator.cpp
    CMakeLists.txt
    Cell.cpp
    CellAllocator.cpp
    ConservativeVector.cpp
    Handle.cpp
    Heap.cpp
    HeapBlock.cpp
    MarkedVector.cpp
    WeakContainer.cpp
)

serenity_lib(LibGC gc)
target_link_libraries(LibGC PRIVATE LibCore)
