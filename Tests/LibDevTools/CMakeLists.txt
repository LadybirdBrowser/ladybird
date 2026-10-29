ladybird_test_suite(LibDevTools)

set(TEST_SOURCES
    TestDevToolsProtocol.cpp
    TestStorageHelpers.cpp
)

target_sources(TestLibDevTools PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibDevTools PRIVATE LibDevTools LibHTTP LibRequests LibThreading LibWeb LibWebView)
