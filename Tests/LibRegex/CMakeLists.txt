set(TEST_SOURCES
    TestRegex.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibRegex LIBS LibRegex)
endforeach()
