include(audio)

set(TEST_SOURCES
    TestAudioDataProvider.cpp
    TestCICP.cpp
    TestDataProviders.cpp
    TestFFmpegAudioNormalization.cpp
    TestFFmpegDemuxer.cpp
    TestH264Decode.cpp
    TestIncrementallyPopulatedStream.cpp
    TestMatroskaDemuxer.cpp
    TestParseMatroska.cpp
    TestPlaybackStream.cpp
    TestVorbisDecode.cpp
    TestVP9Decode.cpp
    TestWav.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibMedia LIBS LibMedia LibFileSystem WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

target_link_libraries(TestFFmpegDemuxer PRIVATE LibThreading)
target_link_libraries(TestIncrementallyPopulatedStream PRIVATE LibThreading)

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_compile_definitions(TestPlaybackStream PRIVATE HAVE_PULSEAUDIO=1)
    target_link_libraries(TestPlaybackStream PRIVATE PkgConfig::PULSEAUDIO)
endif()
