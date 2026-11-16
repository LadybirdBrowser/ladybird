ladybird_test_suite(CameraServer)

set(TEST_SOURCES
    TestCaptureSession.cpp
    TestDeviceList.cpp
    TestFrameConversion.cpp
    TestPictureScaler.cpp
)

target_sources(TestCameraServer PRIVATE ${TEST_SOURCES})
target_link_libraries(TestCameraServer PRIVATE cameraserverservice LibCamera LibCore LibIPC LibThreading)
