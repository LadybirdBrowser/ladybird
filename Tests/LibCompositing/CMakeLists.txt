set(TEST_SOURCES
    BenchmarkForceDarkImageFilter.cpp
    TestAsyncScrollingStateScrollbarMetadata.cpp
    TestAsyncScrollingStateSnapMetadata.cpp
    TestCSSPixels.cpp
    TestDisplayListCommandRuns.cpp
    TestForceDarkImageFilter.cpp
    TestPausedDebuggerOverlay.cpp
    TestScrollSnapSelection.cpp
    TestScrollState.cpp
    TestSmoothScrollAnimation.cpp
    TestTextureUpload.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    set(test_libraries LibCompositing LibGfx LibIPC)
    if (source STREQUAL "BenchmarkForceDarkImageFilter.cpp" OR source STREQUAL "TestForceDarkImageFilter.cpp")
        # These two drive Skia directly, and LibGfx keeps its Skia dependency private.
        list(APPEND test_libraries skia)
    endif()
    ladybird_test("${source}" LibCompositing LIBS ${test_libraries})
endforeach()
