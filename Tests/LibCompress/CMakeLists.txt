set(TEST_SOURCES
    TestBrotli.cpp
    TestDeflate.cpp
    TestGzip.cpp
    TestLzw.cpp
    TestZlib.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibCompress LIBS LibCompress)
endforeach()
