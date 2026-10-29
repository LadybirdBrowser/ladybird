ladybird_test_suite(LibCompress)

set(TEST_SOURCES
    TestDeflate.cpp
    TestZlib.cpp
)

target_sources(TestLibCompress PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCompress PRIVATE LibCompress)
