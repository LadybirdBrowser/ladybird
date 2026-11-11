ladybird_test_suite(LibCompositing)

set(TEST_SOURCES
    TestOverlayFade.cpp
    TestOverlayGeometry.cpp
    TestPausedDebuggerOverlay.cpp
    TestPictureInPictureControls.cpp
    TestScrollRequest.cpp
    TestTextureUpload.cpp
    TestUIOverlays.cpp
    TestVideoFrameHold.cpp
)

target_sources(TestLibCompositing PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCompositing PRIVATE LibCompositing LibGfx LibIPC)
