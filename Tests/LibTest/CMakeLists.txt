set(TEST_SOURCES
    TestNoCrash.cpp
    TestGenerator.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" Test)
endforeach()
