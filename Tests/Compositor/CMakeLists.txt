set(TEST_SOURCES
    TestAsyncScrollTree.cpp
    TestContextState.cpp
    TestScrollbarController.cpp
    TestVSyncScheduler.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx)
endforeach()

# These two drive Skia directly, and the compositor keeps its Skia dependency private.
foreach(source IN ITEMS BenchmarkForceDarkImageFilter.cpp TestForceDarkImageFilter.cpp)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx skia)
endforeach()

if (NOT WIN32)
    ladybird_test("TestTransportPlatform.cpp" Compositor LIBS compositorservice LibCompositing LibGfx LibWeb)
endif()
