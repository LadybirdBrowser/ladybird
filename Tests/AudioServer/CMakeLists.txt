ladybird_test_suite(AudioServer)

set(TEST_SOURCES
    TestCaptureDevices.cpp
    TestPlatformPlaybackStream.cpp
    TestPlaybackStreamMixer.cpp
    TestRemotePlaybackStream.cpp
    TestRemoteRecordStream.cpp
)

target_sources(TestAudioServer PRIVATE ${TEST_SOURCES})
target_link_libraries(TestAudioServer PRIVATE audioserverservice LibAudio LibCore LibIPC LibMedia LibThreading)
