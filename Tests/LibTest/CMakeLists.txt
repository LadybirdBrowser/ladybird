ladybird_test_suite(LibTest)

set(TEST_SOURCES
    TestNoCrash.cpp
    TestGenerator.cpp
    TestSuiteIsolation.cpp
    TestSuiteIsolationPeer.cpp
)

target_sources(TestLibTest PRIVATE ${TEST_SOURCES})
