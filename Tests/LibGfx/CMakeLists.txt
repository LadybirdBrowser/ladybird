set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestColor.cpp
    TestFontCatalog.cpp
    TestFont.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestPath.cpp
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
target_link_libraries(TestFont PRIVATE harfbuzz LibIPC LibThreading)
target_link_libraries(TestSharedFontProvider PRIVATE LibIPC)
target_compile_definitions(TestSharedFontProvider PRIVATE LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR}")

ladybird_test(TestBitmapSequence.cpp LibGfx LIBS LibGfx LibIPC)
ladybird_test(TestShareableBitmap.cpp LibGfx LIBS LibGfx LibIPC)

# Opens LibWeb by name to reach its copy of the Rust graphics crate.
if (APPLE OR LINUX)
    ladybird_test(TestRustCrateCopies.cpp LibGfx LIBS LibGfx LibWeb)
endif()
