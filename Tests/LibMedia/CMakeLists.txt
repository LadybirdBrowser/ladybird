include(audio)

set(TEST_SOURCES
    TestAudioBlockTimingRing.cpp
    TestCICP.cpp
    TestBufferedRanges.cpp
    TestDecodedAudioProducer.cpp
    TestDisplayingVideoSink.cpp
    TestFFmpegAudioNormalization.cpp
    TestFFmpegDemuxer.cpp
    TestH264Decode.cpp
    TestIncrementallyPopulatedStream.cpp
    TestMatroskaDemuxer.cpp
    TestParseMatroska.cpp
    TestPipelineNodes.cpp
    TestRemoteVideoNode.cpp
    TestMediaTime.cpp
    TestPlaybackStream.cpp
    TestVideoEdgeQueue.cpp
    TestVideoFramePool.cpp
    TestVorbisDecode.cpp
    TestTimeRanges.cpp
    TestVP9Decode.cpp
    TestWav.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibMedia LIBS LibMedia LibFileSystem LibSync)
endforeach()

target_link_libraries(TestAudioBlockTimingRing PRIVATE LibThreading)
target_link_libraries(TestDecodedAudioProducer PRIVATE LibGfx)
target_link_libraries(TestDisplayingVideoSink PRIVATE LibGfx)
target_link_libraries(TestH264Decode PRIVATE LibGfx)
target_link_libraries(TestVP9Decode PRIVATE LibGfx)
target_link_libraries(TestFFmpegDemuxer PRIVATE LibThreading)
target_link_libraries(TestMediaTime PRIVATE LibThreading)
target_link_libraries(TestVideoFramePool PRIVATE LibThreading)
target_link_libraries(TestIncrementallyPopulatedStream PRIVATE LibThreading)
target_link_libraries(TestRemoteVideoNode PRIVATE LibThreading LibGfx LibIPC)

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_compile_definitions(TestPlaybackStream PRIVATE HAVE_PULSEAUDIO=1)
    target_link_libraries(TestPlaybackStream PRIVATE PkgConfig::PULSEAUDIO)
endif()
