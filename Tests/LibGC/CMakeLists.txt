set(TEST_SOURCES
    TestGCContainers.cpp
    TestGCIdleCollection.cpp
    TestGCVisitor.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGC LIBS LibGC)
endforeach()
