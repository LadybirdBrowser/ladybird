set(TEST_SOURCES
    TestElasticOverscroll.cpp
    TestOverlayFade.cpp
    TestOverlayGeometry.cpp
    TestPausedDebuggerOverlay.cpp
    TestPictureInPictureControls.cpp
    TestScrollRequest.cpp
    TestScrollState.cpp
    TestSmoothScrollAnimation.cpp
    TestTextureUpload.cpp
    TestUIOverlays.cpp
    TestVideoFrameHold.cpp
    TestWheelGestureIdentity.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibCompositing LIBS LibCompositing LibGfx LibIPC)
endforeach()
