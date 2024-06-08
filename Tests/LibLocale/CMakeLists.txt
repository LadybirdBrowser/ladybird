set(TEST_SOURCES
    TestDateTimeFormat.cpp
    TestDisplayNames.cpp
    TestLocale.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibLocale LIBS LibLocale)

    get_filename_component(target "${source}" NAME_WLE)
endforeach()
