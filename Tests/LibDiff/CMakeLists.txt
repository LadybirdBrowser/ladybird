set(TEST_SOURCES
    TestDiff.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDiff LIBS LibDiff)
endforeach()
