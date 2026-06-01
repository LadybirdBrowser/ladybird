set(TEST_SOURCES
    TestHistoryStore.cpp
    TestHSTSStore.cpp
    TestWebViewURL.cpp
)

ladybird_test_suite(TestWebView LibWebView SOURCES ${TEST_SOURCES} LIBS LibDatabase LibWebView LibURL)
