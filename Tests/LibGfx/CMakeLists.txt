ladybird_test_suite(LibGfx)

set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestColor.cpp
    TestFont.cpp
    TestFontCatalog.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestRect.cpp
    TestSharedFontProvider.cpp
    TestWOFF.cpp
    TestWOFF2.cpp
)

list(APPEND TEST_SOURCES TestBitmapSequence.cpp TestShareableBitmap.cpp)

# Opens the LibGfx and LibWeb shared libraries by name to reach their copies of the Rust graphics crate.
if (BUILD_SHARED_LIBS AND (APPLE OR LINUX))
    list(APPEND TEST_SOURCES TestRustCrateCopies.cpp)
    target_link_libraries(TestLibGfx PRIVATE LibWeb)
endif()

target_sources(TestLibGfx PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibGfx PRIVATE LibGfx LibImageDecoders LibIPC LibThreading harfbuzz)
set_property(SOURCE TestSharedFontProvider.cpp APPEND PROPERTY COMPILE_DEFINITIONS LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR}")
