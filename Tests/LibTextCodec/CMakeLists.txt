ladybird_test_suite(LibTextCodec)

set(TEST_SOURCES
    TestTextDecoders.cpp
    TestTextEncoders.cpp
)

target_sources(TestLibTextCodec PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibTextCodec PRIVATE LibTextCodec)
