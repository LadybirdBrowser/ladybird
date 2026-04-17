set(TEST_SOURCES
    TestCacheUtilities.cpp
    TestHTTPUtils.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibHTTP)
endforeach()

ladybird_test("TestCacheIndex.cpp" LibWeb LIBS LibHTTP LibDatabase)
