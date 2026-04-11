set(TEST_SOURCES
    TestHistoryStore.cpp
    TestWebViewURL.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebView LIBS LibDatabase LibWebView LibURL)
endforeach()
