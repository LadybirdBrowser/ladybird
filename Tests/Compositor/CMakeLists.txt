set(TEST_SOURCES
    TestAsyncScrollTree.cpp
    TestContextState.cpp
    TestScrollbarController.cpp
    TestVSyncScheduler.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx)
endforeach()

# These drive Skia directly, and the compositor keeps its Skia dependency private.
foreach(source IN ITEMS BenchmarkForceDarkImageFilter.cpp TestForceDarkImageFilter.cpp TestYUVData.cpp)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx skia)
endforeach()

if (APPLE)
    ladybird_test(TestVideoSurfaceImage.mm Compositor LIBS compositorservice LibCompositing LibGfx skia)
    target_link_libraries(TestVideoSurfaceImage PRIVATE "-framework CoreVideo")
endif()

if (NOT WIN32)
    ladybird_test("TestTransportPlatform.cpp" Compositor LIBS compositorservice LibCompositing LibGfx LibWeb)
endif()
