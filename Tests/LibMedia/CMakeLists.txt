set(TEST_SOURCES
    TestH264Decode.cpp
    TestParseMatroska.cpp
    TestVP9Decode.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibMedia LIBS LibMedia)
endforeach()
