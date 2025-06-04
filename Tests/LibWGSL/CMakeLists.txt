set(TEST_SOURCES
    TestWGSLLexer.cpp
    TestWGSLPreprocessor.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibWGSL LIBS LibWGSL)
endforeach()
