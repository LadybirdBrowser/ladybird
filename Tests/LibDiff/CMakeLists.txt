set(TEST_SOURCES
    TestDiff.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibDiff LIBS LibDiff)
endforeach()
