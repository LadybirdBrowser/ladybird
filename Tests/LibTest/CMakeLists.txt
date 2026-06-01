set(TEST_SOURCES
    TestNoCrash.cpp
    TestGenerator.cpp
)

ladybird_test_suite(TestLibTest Test SOURCES ${TEST_SOURCES})
