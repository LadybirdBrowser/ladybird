set(TEST_SOURCES
    TestADTSDemuxer.cpp
    TestADTSFrameHeader.cpp
    TestADTSReader.cpp
    TestAudioBlockTimingRing.cpp
    TestAudioDiscardIntervals.cpp
    TestAudioMixer.cpp
    TestBitReader.cpp
    TestBufferedRanges.cpp
    TestCICP.cpp
    TestCodecConfigurations.cpp
    TestCodecParameters.cpp
    TestContainerID.cpp
    TestDecodedAudioProducer.cpp
    TestDecodedVideoProducer.cpp
    TestDecoderRegistry.cpp
    TestDemuxerRegistry.cpp
    TestDisplayingVideoSink.cpp
    TestFFmpegAudioNormalization.cpp
    TestFLACDemuxer.cpp
    TestID3.cpp
    TestIncrementallyPopulatedStream.cpp
    TestISOBMFF.cpp
    TestMatroskaDemuxer.cpp
    TestFrameScanTimeline.cpp
    TestMP3Demuxer.cpp
    TestMP3FrameHeader.cpp
    TestMP3Reader.cpp
    TestMP3StreamInfo.cpp
    TestNonReferenceFrameDetector.cpp
    TestMediaSupport.cpp
    TestMediaTime.cpp
    TestOggDemuxer.cpp
    TestParseMatroska.cpp
    TestPipelineNodes.cpp
    TestPlaybackManager.cpp
    TestRemoteVideoDecoder.cpp
    TestRemoteVideoNode.cpp
    TestSourceBufferDemuxer.cpp
    TestTimeRanges.cpp
    TestVideoEdgeQueue.cpp
    TestVideoFramePool.cpp
    TestVorbisDecode.cpp
    TestVP9Decode.cpp
    TestWav.cpp
    TestWAVDemuxer.cpp
)

if (APPLE)
    list(APPEND TEST_SOURCES
        TestAudioToolboxDecode.cpp
        TestVideoToolboxDecode.cpp
    )
endif()
if (LINUX)
    list(APPEND TEST_SOURCES
        TestH264Decode.cpp
        TestSystemFFmpeg.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibMedia LIBS LibAudio LibMedia LibFileSystem)
endforeach()

target_link_libraries(TestAudioBlockTimingRing PRIVATE LibThreading)
target_link_libraries(TestDecodedAudioProducer PRIVATE LibGfx)
target_link_libraries(TestDecodedVideoProducer PRIVATE LibThreading)
target_link_libraries(TestDisplayingVideoSink PRIVATE LibGfx)
target_link_libraries(TestVP9Decode PRIVATE LibGfx)
target_link_libraries(TestADTSDemuxer PRIVATE LibThreading)
target_link_libraries(TestFLACDemuxer PRIVATE LibThreading)
target_link_libraries(TestMP3Demuxer PRIVATE LibThreading)
target_link_libraries(TestOggDemuxer PRIVATE LibThreading LibGfx)
target_link_libraries(TestWAVDemuxer PRIVATE LibThreading)
target_link_libraries(TestISOBMFF PRIVATE LibThreading)
target_link_libraries(TestMediaTime PRIVATE LibThreading)
target_link_libraries(TestVideoFramePool PRIVATE LibThreading LibIPC)
target_link_libraries(TestIncrementallyPopulatedStream PRIVATE LibThreading)
target_link_libraries(TestRemoteVideoDecoder PRIVATE LibThreading LibIPC LibMediaCodecs)
target_link_libraries(TestRemoteVideoNode PRIVATE LibThreading LibGfx LibIPC)

if (LINUX)
    target_link_libraries(TestH264Decode PRIVATE LibGfx)
    find_library(LADYBIRD_BUNDLED_LIBAVCODEC NAMES ${AVCODEC_LIBRARIES} PATHS ${AVCODEC_LIBRARY_DIRS} NO_DEFAULT_PATH REQUIRED)
    target_compile_definitions(TestSystemFFmpeg PRIVATE LADYBIRD_BUNDLED_LIBAVCODEC_PATH="${LADYBIRD_BUNDLED_LIBAVCODEC}")
endif()
