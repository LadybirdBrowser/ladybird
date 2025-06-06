set(TEST_SOURCES
    TestWGSLLexer.cpp
    TestWGSLParser.cpp
    TestWGSLPreprocessor.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWGSL LIBS LibWGSL)
endforeach()
