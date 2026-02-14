set(TEST_SOURCES
    TestDatabase.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDatabase LIBS LibDatabase)
endforeach()
