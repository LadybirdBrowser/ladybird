set(TEST_SOURCES
    TestNoCrash.cpp
    TestGenerator.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Test)
endforeach()
