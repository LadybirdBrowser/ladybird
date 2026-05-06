set(TEST_SOURCES
    TestGCContainers.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGC LIBS LibGC)
endforeach()
