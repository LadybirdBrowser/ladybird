ladybird_test_suite(LibUnicode)

set(TEST_SOURCES
    TestBidi.cpp
    TestDisplayNames.cpp
    TestFullwidthMapping.cpp
    TestIDNA.cpp
    TestLocale.cpp
    TestSegmenter.cpp
    TestTimeZone.cpp
    TestUnicodeCharacterTypes.cpp
    TestUnicodeNormalization.cpp
)

target_sources(TestLibUnicode PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibUnicode PRIVATE LibUnicode LibThreading ICU::i18n ICU::uc)
