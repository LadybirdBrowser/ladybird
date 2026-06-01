set(TEST_SOURCES
    TestCacheUtilities.cpp
    TestHSTSPolicy.cpp
    TestHSTSPreloadData.cpp
    TestHTTPUtils.cpp
)

ladybird_test_suite(TestHTTP LibHTTP SOURCES ${TEST_SOURCES} TestCacheIndex.cpp TestDiskCache.cpp LIBS LibHTTP LibDatabase LibURL)
