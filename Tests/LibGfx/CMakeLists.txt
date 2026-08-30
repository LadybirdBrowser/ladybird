set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestBitmapExport.cpp
    TestColor.cpp
    TestFontCatalog.cpp
    TestFont.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestQuad.cpp
    TestRect.cpp
    TestSharedFontProvider.cpp
    TestWOFF.cpp
    TestWOFF2.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGfx LIBS LibGfx)
endforeach()

target_link_libraries(BenchmarkJPEGLoader PRIVATE LibImageDecoders)
target_link_libraries(TestImageDecoder PRIVATE LibImageDecoders)
target_link_libraries(TestImageWriter PRIVATE LibImageDecoders)
target_link_libraries(TestFont PRIVATE harfbuzz)
target_link_libraries(TestSharedFontProvider PRIVATE LibIPC)
target_compile_definitions(TestSharedFontProvider PRIVATE LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR}")

ladybird_test(TestBitmapSequence.cpp LibGfx LIBS LibGfx LibIPC)
ladybird_test(TestShareableBitmap.cpp LibGfx LIBS LibGfx LibIPC)
ladybird_test(TestYUVData.cpp LibGfx LIBS LibGfx skia)
