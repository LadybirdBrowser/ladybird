set(TEST_SOURCES
    TestCaptureDevices.cpp
    TestPlatformPlaybackStream.cpp
    TestPlaybackStreamMixer.cpp
    TestRemotePlaybackStream.cpp
    TestRemoteRecordStream.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" AudioServer LIBS audioserverservice LibCore LibIPC LibMedia LibThreading)
endforeach()
