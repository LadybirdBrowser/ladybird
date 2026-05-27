set(TEST_SOURCES
    TestCacheUtilities.cpp
    TestHSTSPolicy.cpp
    TestHSTSPreloadData.cpp
    TestHTTPUtils.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibHTTP)
endforeach()

ladybird_test("TestCacheIndex.cpp" LibWeb LIBS LibHTTP LibDatabase)
ladybird_test("TestDiskCache.cpp" LibWeb LIBS LibHTTP LibURL)
ladybird_test("TestMemoryCache.cpp" LibWeb LIBS LibHTTP LibURL)
