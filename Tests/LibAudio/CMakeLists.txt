set(TEST_SOURCES
    TestPlaybackStream.cpp
    TestSharedAudioFrameRing.cpp
    TestSpscAudioFrameRing.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibAudio LIBS LibAudio LibCore LibThreading)
endforeach()
