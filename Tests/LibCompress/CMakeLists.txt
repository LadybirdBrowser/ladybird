set(TEST_SOURCES
    TestDeflate.cpp
    TestGzip.cpp
    TestLzw.cpp
    TestPackBits.cpp
    TestZlib.cpp
)

ladybird_test_suite(TestCompress LibCompress SOURCES ${TEST_SOURCES} LIBS LibCompress)
