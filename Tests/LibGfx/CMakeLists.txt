set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestBitmapExport.cpp
    TestColor.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestQuad.cpp
    TestRect.cpp
    TestWOFF.cpp
    TestWOFF2.cpp
)

ladybird_test_suite(TestGfx LibGfx SOURCES ${TEST_SOURCES} TestYUVData.cpp LIBS LibGfx skia)
