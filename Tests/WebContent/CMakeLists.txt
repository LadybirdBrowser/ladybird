ladybird_test_suite(WebContent)

set(TEST_SOURCES
    TestWorkerPageHost.cpp
)

target_sources(TestWebContent PRIVATE ${TEST_SOURCES})
target_link_libraries(TestWebContent PRIVATE webcontentservice LibCore LibIPC LibJS LibThreading LibWeb LibWebCommon)
