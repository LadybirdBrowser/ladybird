set(TEST_SOURCES
    TestBackgroundAction.cpp
    TestThread.cpp
)

ladybird_test_suite(TestThreading LibThreading SOURCES ${TEST_SOURCES} LIBS LibThreading LibSync)
