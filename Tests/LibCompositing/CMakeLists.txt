ladybird_test_suite(LibCompositing)

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

target_sources(TestLibCompositing PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCompositing PRIVATE LibCompositing LibGfx LibIPC)
