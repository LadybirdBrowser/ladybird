include(audio)

set(TEST_SOURCES
    TestCICP.cpp
    TestBufferedRanges.cpp
    TestDataProducers.cpp
    TestDecodedAudioProducer.cpp
    TestFFmpegAudioNormalization.cpp
    TestFFmpegDemuxer.cpp
    TestH264Decode.cpp
    TestIncrementallyPopulatedStream.cpp
    TestMatroskaDemuxer.cpp
    TestParseMatroska.cpp
    TestPlaybackStream.cpp
    TestVorbisDecode.cpp
    TestTimeRanges.cpp
    TestVP9Decode.cpp
    TestWav.cpp
)

ladybird_test_suite(TestMedia LibMedia SOURCES ${TEST_SOURCES} LIBS LibMedia LibFileSystem LibSync LibThreading)

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_compile_definitions(TestMedia PRIVATE HAVE_PULSEAUDIO=1)
    target_link_libraries(TestMedia PRIVATE PkgConfig::PULSEAUDIO)
endif()
