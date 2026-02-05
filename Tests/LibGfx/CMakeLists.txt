set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestColor.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestImmutableBitmap.cpp
    TestQuad.cpp
    TestRect.cpp
    TestWOFF.cpp
    TestWOFF2.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGfx LIBS LibGfx)
endforeach()
