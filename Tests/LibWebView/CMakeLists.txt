set(TEST_SOURCES
    TestWebViewURL.cpp
    TestFontPlugin.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibWebView LIBS LibWebView LibURL)
endforeach()
