ladybird_test_suite(LibMedia)

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
    TestFrameScanTimeline.cpp
    TestID3.cpp
    TestIncrementallyPopulatedStream.cpp
    TestISOBMFF.cpp
    TestMatroskaDemuxer.cpp
    TestMediaSupport.cpp
    TestMediaTime.cpp
    TestMP3Demuxer.cpp
    TestMP3FrameHeader.cpp
    TestMP3Reader.cpp
    TestMP3StreamInfo.cpp
    TestNonReferenceFrameDetector.cpp
    TestOggDemuxer.cpp
    TestParseMatroska.cpp
    TestPipelineNodes.cpp
    TestPlaybackManager.cpp
    TestRemoteVideoDecoder.cpp
    TestRemoteVideoNode.cpp
    TestSourceBufferDemuxer.cpp
    TestTimeRanges.cpp
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
if (WIN32)
    list(APPEND TEST_SOURCES
        TestWindowsMediaFoundationAudioDecode.cpp
        TestWindowsMediaFoundationDecode.cpp
    )
endif()
if (LINUX)
    list(APPEND TEST_SOURCES TestH264Decode.cpp)

    # Loads the bundled libavcodec in place of a system one, which needs it to be a shared library.
    find_library(LADYBIRD_BUNDLED_LIBAVCODEC NAMES ${AVCODEC_LIBRARIES} PATHS ${AVCODEC_LIBRARY_DIRS} NO_DEFAULT_PATH REQUIRED)
    if (LADYBIRD_BUNDLED_LIBAVCODEC MATCHES "\\${CMAKE_SHARED_LIBRARY_SUFFIX}$")
        list(APPEND TEST_SOURCES TestSystemFFmpeg.cpp)
    endif()
endif()

target_sources(TestLibMedia PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibMedia PRIVATE LibAudio LibFileSystem LibGfx LibIPC LibMedia LibMediaCodecs LibThreading)

if ("TestSystemFFmpeg.cpp" IN_LIST TEST_SOURCES)
    set_property(SOURCE TestSystemFFmpeg.cpp APPEND PROPERTY COMPILE_DEFINITIONS LADYBIRD_BUNDLED_LIBAVCODEC_PATH="${LADYBIRD_BUNDLED_LIBAVCODEC}")
endif()
