set(URL_TEST_SOURCES
    TestURL.cpp
    TestURLPattern.cpp
    TestURLPatternConstructorStringParser.cpp
    TestPublicSuffix.cpp
)

foreach(source IN LISTS URL_TEST_SOURCES)
    ladybird_test("${source}" LibURL LIBS LibURL LibRegex)
endforeach()
