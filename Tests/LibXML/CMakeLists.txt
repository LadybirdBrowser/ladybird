set(TEST_SOURCES
    TestParser.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibXML LIBS LibXML)
endforeach()
