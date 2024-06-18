set(TEST_SOURCES
    TestEmoji.cpp
    TestIDNA.cpp
    TestSegmentation.cpp
    TestUnicodeCharacterTypes.cpp
    TestUnicodeNormalization.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibUnicode LIBS LibUnicode)
endforeach()
