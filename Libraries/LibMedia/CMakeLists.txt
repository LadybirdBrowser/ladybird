set(SOURCES
    Audio/AudioRingBuffer.cpp
    Audio/WSOLAAlgorithm.cpp
    Audio/WSOLAInternals.cpp
    Audio/WSOLATimeStretcher.cpp
    AudioDiscardIntervals.cpp
    Codecs/FLAC.cpp
    Codecs/Opus.cpp
    Codecs/Vorbis.cpp
    ContainerID.cpp
    Containers/ADTS/ADTSDemuxer.cpp
    Containers/ADTS/FrameHeader.cpp
    Containers/ADTS/Reader.cpp
    Containers/FLAC/FLACDemuxer.cpp
    Containers/FLAC/Reader.cpp
    Containers/FrameScanTimeline.cpp
    Containers/ID3.cpp
    Containers/ISOBMFF/FragmentSampleIterator.cpp
    Containers/ISOBMFF/ISOBMFFDemuxer.cpp
    Containers/ISOBMFF/SampleIterator.cpp
    Containers/ISOBMFF/Reader.cpp
    Containers/ISOBMFF/Streamer.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    Containers/Matroska/SampleIterator.cpp
    Containers/Matroska/Streamer.cpp
    Containers/Matroska/TrackBlockContext.cpp
    Containers/MP3/FrameHeader.cpp
    Containers/MP3/MP3Demuxer.cpp
    Containers/MP3/Reader.cpp
    Containers/MP3/StreamInfo.cpp
    Containers/Ogg/OggDemuxer.cpp
    Containers/Ogg/Reader.cpp
    Containers/WAV/Reader.cpp
    Containers/WAV/WAVDemuxer.cpp
    DecodeAudioStream.cpp
    DecoderRegistrations.cpp
    DemuxerRegistry.cpp
    DemuxerScanState.cpp
    IncrementallyPopulatedStream.cpp
    MediaSourceExtensions/ISOBMFFByteStreamParser.cpp
    MediaSourceExtensions/SourceBufferDemuxer.cpp
    MediaSourceExtensions/SourceBufferProcessor.cpp
    MediaSourceExtensions/TrackBuffer.cpp
    MediaSourceExtensions/WebMByteStreamParser.cpp
    MediaSupport.cpp
    MediaTime.cpp
    MonotonicMediaClock.cpp
    PlaybackManager.cpp
    PlaybackStates/BufferingStateHandler.cpp
    PlaybackStates/EndedStateHandler.cpp
    PlaybackStates/PausedStateHandler.cpp
    PlaybackStates/PlaybackStateHandler.cpp
    PlaybackStates/PlayingStateHandler.cpp
    PlaybackStates/ResumingStateHandler.cpp
    PlaybackStates/StartingStateHandler.cpp
    PlaybackStates/SuspendedStateHandler.cpp
    Processors/AudioMixer.cpp
    Processors/AudioTimeStretchProcessor.cpp
    Producers/DecodedAudioProducer.cpp
    Producers/DecodedVideoProducer.cpp
    Producers/RemoteVideoProducer.cpp
    RemoteVideoDecoder.cpp
    Sinks/AudioPlaybackSink.cpp
    Sinks/DisplayingVideoSink.cpp
    Sinks/RemoteVideoSink.cpp
    TimeRanges.cpp
    Track.cpp
    VideoEdgeQueue.cpp
    VideoPresentation/VideoPresentationClientConnection.cpp
    VideoPresentation/VideoPresentationServerConnection.cpp
)

set(GENERATED_SOURCES
    VideoPresentation/VideoPresentationClientEndpoint.h
    VideoPresentation/VideoPresentationServerEndpoint.h
)

if (APPLE)
    list(APPEND SOURCES
        AudioToolbox/AudioToolboxAudioDecoder.cpp
    )
endif()

if (WIN32)
    list(APPEND SOURCES
        WindowsMediaFoundation/WindowsMediaFoundationAudioDecoder.cpp
    )
endif()

compile_ipc(VideoPresentation/VideoPresentationClient.ipc VideoPresentation/VideoPresentationClientEndpoint.h)
compile_ipc(VideoPresentation/VideoPresentationServer.ipc VideoPresentation/VideoPresentationServerEndpoint.h)

ladybird_lib(LibMedia media EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibIPC LibGfx LibThreading LibUnicode)
target_link_libraries(LibMedia PUBLIC LibAudio LibMediaCodecs)

if (APPLE)
    target_link_libraries(LibMedia PRIVATE "-framework AudioToolbox")
endif()

if (WIN32)
    target_link_libraries(LibMedia PRIVATE mfplat mfuuid)
endif()

target_sources(LibMedia PRIVATE
        FFmpeg/FFmpegAudioConverter.cpp
        FFmpeg/FFmpegAudioDecoder.cpp
        FFmpeg/FFmpegChannelLayout.cpp
)

if (NOT ANDROID)
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::LIBSWRESAMPLE)
else()
    target_include_directories(LibMedia PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(LibMedia PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(LibMedia PRIVATE ${FFMPEG_LIBRARIES})
endif()

if (LINUX)
    target_sources(LibMedia PRIVATE
        FFmpeg/SystemFFmpegDecoders.cpp
    )
    target_link_libraries(LibMedia PRIVATE ${CMAKE_DL_LIBS})
endif()
