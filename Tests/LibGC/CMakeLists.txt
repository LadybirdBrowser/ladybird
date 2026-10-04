set(TEST_SOURCES
    TestBlockAllocator.cpp
    TestExternalEntityTable.cpp
    TestGCContainers.cpp
    TestGCHeapGroup.cpp
    TestGCIdleCollection.cpp
    TestGCIncrementalSweep.cpp
    TestPrimitiveStorage.cpp
    TestGCVisitor.cpp
    TestNanBoxedValue.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGC LIBS LibGC)
endforeach()
