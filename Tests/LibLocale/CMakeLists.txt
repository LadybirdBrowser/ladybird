set(TEST_SOURCES
    TestDisplayNames.cpp
    TestLocale.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibLocale LIBS LibLocale)
endforeach()
