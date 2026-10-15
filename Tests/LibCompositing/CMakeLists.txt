set(TEST_SOURCES
    TestElasticOverscroll.cpp
    TestOverlayGeometry.cpp
    TestPausedDebuggerOverlay.cpp
    TestScrollState.cpp
    TestSmoothScrollAnimation.cpp
    TestTextureUpload.cpp
    TestUIOverlays.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibCompositing LIBS LibCompositing LibGfx LibIPC)
endforeach()
