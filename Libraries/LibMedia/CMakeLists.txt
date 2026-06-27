include(audio)

set(SOURCES
    Audio/AudioDevices.cpp
    Audio/AudioRingBuffer.cpp
    Audio/NullPlaybackStream.cpp
    Audio/PlaybackStream.cpp
    Audio/WSOLAAlgorithm.cpp
    Audio/WSOLAInternals.cpp
    Audio/WSOLATimeStretcher.cpp
    Codecs/FLAC.cpp
    Codecs/Opus.cpp
    Codecs/Vorbis.cpp
    Containers/ConstantBitrateContainerNavigator.cpp
    Containers/FLACNavigator.cpp
    Containers/IndexedContainerNavigator.cpp
    Containers/MP3Navigator.cpp
    Containers/OggNavigator.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    Containers/Matroska/SampleIterator.cpp
    Containers/Matroska/Streamer.cpp
    DecodeAudioStream.cpp
    IncrementallyPopulatedStream.cpp
    MediaTime.cpp
    MonotonicMediaClock.cpp
    PlaybackManager.cpp
    PlaybackStates/BufferingStateHandler.cpp
    PlaybackStates/PlayingStateHandler.cpp
    PlaybackStates/StartingStateHandler.cpp
    PlaybackStates/PausedStateHandler.cpp
    PlaybackStates/PlaybackStateHandler.cpp
    PlaybackStates/ResumingStateHandler.cpp
    Processors/AudioMixer.cpp
    Processors/AudioTimeStretchProcessor.cpp
    Producers/DecodedAudioProducer.cpp
    Producers/DecodedVideoProducer.cpp
    Producers/RemoteVideoProducer.cpp
    Sinks/AudioPlaybackSink.cpp
    Sinks/DisplayingVideoSink.cpp
    Sinks/RemoteVideoSink.cpp
    VideoPresentation/PresentedFramePage.cpp
    VideoPresentation/VideoPresentationClientConnection.cpp
    VideoPresentation/VideoPresentationServerConnection.cpp
    TimeRanges.cpp
    VideoEdgeQueue.cpp
    VideoFrame.cpp
    VideoFrameHandle.cpp
    VideoFramePool.cpp
)

set(GENERATED_SOURCES
    VideoPresentation/VideoPresentationClientEndpoint.h
    VideoPresentation/VideoPresentationServerEndpoint.h
)

compile_ipc(VideoPresentation/VideoPresentationClient.ipc VideoPresentation/VideoPresentationClientEndpoint.h)
compile_ipc(VideoPresentation/VideoPresentationServer.ipc VideoPresentation/VideoPresentationServerEndpoint.h)

ladybird_lib(LibMedia media EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibIPC LibGfx LibSync LibThreading LibUnicode)

target_sources(LibMedia PRIVATE
        FFmpeg/FFmpegAudioConverter.cpp
        FFmpeg/FFmpegAudioDecoder.cpp
        FFmpeg/FFmpegDemuxer.cpp
        FFmpeg/FFmpegHelpers.cpp
        FFmpeg/FFmpegIOContext.cpp
        FFmpeg/FFmpegVideoDecoder.cpp
)

if (NOT ANDROID)
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC PkgConfig::AVFORMAT PkgConfig::AVUTIL PkgConfig::LIBSWRESAMPLE)
else()
    target_include_directories(LibMedia PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(LibMedia PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(LibMedia PRIVATE ${FFMPEG_LIBRARIES})
endif()

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_sources(LibMedia PRIVATE
        Audio/PlaybackStreamPulseAudio.cpp
        Audio/PulseAudioWrappers.cpp
    )
    target_compile_definitions(LibMedia PRIVATE LIBMEDIA_AUDIO_BACKEND=1)
    target_link_libraries(LibMedia PRIVATE PkgConfig::PULSEAUDIO)
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "AUDIO_UNIT")
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamAudioUnit.cpp)
    target_compile_definitions(LibMedia PRIVATE LIBMEDIA_AUDIO_BACKEND=1)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    target_link_libraries(LibMedia PRIVATE ${AUDIO_TOOLBOX})
elseif(LADYBIRD_AUDIO_BACKEND STREQUAL "WASAPI")
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamWasapi.cpp)
    target_compile_definitions(LibMedia PRIVATE LIBMEDIA_AUDIO_BACKEND=1)
    target_link_libraries(LibMedia PRIVATE ole32 avrt winmm)
elseif (DEFINED LADYBIRD_AUDIO_BACKEND)
    message(FATAL_ERROR "Please update ${CMAKE_CURRENT_LIST_FILE} for audio backend ${LADYBIRD_AUDIO_BACKEND}")
endif()
