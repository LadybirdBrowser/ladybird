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

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibUnicode LIBS LibUnicode)
endforeach()

target_link_libraries(TestLocale PRIVATE LibThreading ICU::i18n ICU::uc)
