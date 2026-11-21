ladybird_test_suite(LibAudio)

set(TEST_SOURCES
    TestPlaybackStream.cpp
    TestSharedAudioFrameRing.cpp
    TestSpscAudioFrameRing.cpp
)

if (APPLE)
    list(APPEND TEST_SOURCES TestCoreAudioChannelLayout.cpp)
endif()

target_sources(TestLibAudio PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibAudio PRIVATE LibAudio LibCore LibThreading)
