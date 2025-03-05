set(URL_TEST_SOURCES
    TestURL.cpp
    TestURLPatternConstructorStringParser.cpp
)

foreach(source IN LISTS URL_TEST_SOURCES)
    serenity_test("${source}" LibURL LIBS LibURL)
endforeach()
