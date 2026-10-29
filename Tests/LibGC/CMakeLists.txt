ladybird_test_suite(LibGC)

set(TEST_SOURCES
    TestBlockAllocator.cpp
    TestExternalEntityTable.cpp
    TestGCContainers.cpp
    TestGCFreeList.cpp
    TestGCHeapGroup.cpp
    TestGCIdleCollection.cpp
    TestGCIncrementalSweep.cpp
    TestGCVisitor.cpp
    TestNanBoxedValue.cpp
    TestPrimitiveStorage.cpp
)

target_sources(TestLibGC PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibGC PRIVATE LibGC)
