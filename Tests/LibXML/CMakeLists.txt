set(TEST_SOURCES
    TestParser.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibXML LIBS LibXML)
endforeach()
