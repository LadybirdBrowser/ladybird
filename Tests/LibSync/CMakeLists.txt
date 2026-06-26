set(TEST_SOURCES
    TestWeakable.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibSync LIBS LibSync LibThreading)
endforeach()
