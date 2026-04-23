set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestColor.cpp
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

# TestImageDecoder uses zlib's crc32() directly when hand-building PNG fixtures.
ladybird_test(TestImageDecoder.cpp LibGfx LIBS LibGfx ZLIB::ZLIB)
