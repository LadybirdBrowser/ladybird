set(TEST_SOURCES
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestSessionHistory.cpp
    TestWebViewURL.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebView LIBS LibDatabase LibWebView LibURL)
endforeach()
