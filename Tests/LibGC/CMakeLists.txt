ladybird_test_suite(LibGC)

set(TEST_SOURCES
    TestBlockAllocator.cpp
    TestCompressedPointer.cpp
    TestExternalEntityTable.cpp
    TestGCContainers.cpp
    TestGCEphemeron.cpp
    TestGCFreeList.cpp
    TestGCFunction.cpp
    TestGCHeapGroup.cpp
    TestGCIdleCollection.cpp
    TestGCIncrementalSweep.cpp
    TestGCThreads.cpp
    TestGCTypeIsolation.cpp
    TestGCVisitor.cpp
    TestMember.cpp
    TestNanBoxedValue.cpp
    TestPrimitiveStorage.cpp
)

target_sources(TestLibGC PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibGC PRIVATE LibGC LibThreading)
