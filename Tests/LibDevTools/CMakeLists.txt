set(TEST_SOURCES
    TestDevToolsProtocol.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDevTools LIBS LibDevTools LibHTTP LibRequests LibWeb LibWebView)
endforeach()
