set(TEST_SOURCES
    TestGCContainers.cpp
    TestGCHeapGroup.cpp
    TestGCIdleCollection.cpp
    TestPrimitiveStorage.cpp
    TestGCVisitor.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGC LIBS LibGC)
endforeach()
