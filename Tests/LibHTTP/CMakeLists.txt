ladybird_test_suite(LibHTTP)

set(TEST_SOURCES
    TestCacheIndex.cpp
    TestCacheUtilities.cpp
    TestDiskCache.cpp
    TestHSTSPolicy.cpp
    TestHSTSPreloadData.cpp
    TestHTTPUtils.cpp
    TestMemoryCache.cpp
    TestProxy.cpp
    TestStructuredFields.cpp
)

target_sources(TestLibHTTP PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibHTTP PRIVATE LibDatabase LibHTTP LibURL)
