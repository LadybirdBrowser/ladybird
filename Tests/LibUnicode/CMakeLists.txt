set(TEST_SOURCES
    TestBidi.cpp
    TestDisplayNames.cpp
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
