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
target_link_libraries(TestFont PRIVATE harfbuzz LibIPC LibThreading skia)
target_link_libraries(TestSharedFontProvider PRIVATE LibIPC)
target_compile_definitions(TestSharedFontProvider PRIVATE LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR}")

ladybird_test(TestBitmapSequence.cpp LibGfx LIBS LibGfx LibIPC)
ladybird_test(TestShareableBitmap.cpp LibGfx LIBS LibGfx LibIPC)
ladybird_test(TestFilter.cpp LibGfx LIBS LibGfx skia)
ladybird_test(TestPaintingSurface.cpp LibGfx LIBS LibGfx skia)

# Opens LibWeb by name to reach its copy of the Rust graphics crate.
if (APPLE OR LINUX)
    ladybird_test(TestRustCrateCopies.cpp LibGfx LIBS LibGfx LibWeb)
endif()
