set(TEST_SOURCES
    TestTextDecoders.cpp
    TestTextEncoders.cpp
)

ladybird_test_suite(TestTextCodec LibTextCodec SOURCES ${TEST_SOURCES} LIBS LibTextCodec)
