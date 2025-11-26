set(TEST_SOURCES
    TestHTTPUtils.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibHTTP)
endforeach()
