ladybird_test_suite(Compositor)

set(TEST_SOURCES
    TestAsyncScrollTree.cpp
    TestBitmapExport.cpp
    TestCanvasSurfaceRegistry.cpp
    TestContextState.cpp
    TestScrollbarController.cpp
    TestVideoFrameHolds.cpp
    TestVSyncScheduler.cpp
)

# These drive Skia directly, and the compositor keeps its Skia dependency private.
list(APPEND TEST_SOURCES
    BenchmarkForceDarkImageFilter.cpp
    TestDisplayListImageFilters.cpp
    TestForceDarkImageFilter.cpp
    TestPaintingSurface.cpp
    TestYUVData.cpp
)

if (APPLE)
    list(APPEND TEST_SOURCES TestVideoSurfaceImage.mm)
    target_link_libraries(TestCompositor PRIVATE "-framework CoreVideo")
endif()

target_sources(TestCompositor PRIVATE ${TEST_SOURCES})
target_link_libraries(TestCompositor PRIVATE compositorservice LibCompositing LibGfx skia)
