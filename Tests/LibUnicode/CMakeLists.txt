set(TEST_SOURCES
    TestDisplayNames.cpp
    TestIDNA.cpp
    TestLocale.cpp
    TestSegmenter.cpp
    TestTimeZone.cpp
    TestUnicodeCharacterTypes.cpp
    TestUnicodeNormalization.cpp
)

ladybird_test_suite(TestUnicode LibUnicode SOURCES ${TEST_SOURCES} LIBS LibUnicode)
