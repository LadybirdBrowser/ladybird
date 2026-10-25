set(TEST_SOURCES
    TestDevToolsProtocol.cpp
    TestStorageHelpers.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDevTools LIBS LibDevTools LibHTTP LibRequests LibThreading LibWeb LibWebView)
endforeach()
