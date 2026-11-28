ladybird_test_suite(LibRequests)

set(TEST_SOURCES
    TestRequestClient.cpp
)

target_sources(TestLibRequests PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibRequests PRIVATE LibHTTP LibIPC LibRequests LibThreading LibURL)
