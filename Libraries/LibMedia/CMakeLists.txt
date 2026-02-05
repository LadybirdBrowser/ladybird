include(audio)

include(ffmpeg)

set(SOURCES
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    IncrementallyPopulatedStream.cpp
    PlaybackManager.cpp
    PlaybackStates/PausedStateHandler.cpp
    PlaybackStates/PlaybackStateHandler.cpp
    PlaybackStates/ResumingStateHandler.cpp
    Providers/AudioDataProvider.cpp
    Providers/GenericTimeProvider.cpp
    Providers/VideoDataProvider.cpp
    Sinks/AudioMixingSink.cpp
    Sinks/DisplayingVideoSink.cpp
    TimedImage.cpp
    VideoFrame.cpp
)

ladybird_lib(LibMedia media EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibIPC LibGfx LibThreading LibUnicode)

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
    target_link_libraries(LibMedia PRIVATE PkgConfig::PULSEAUDIO)
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "AUDIO_UNIT")
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamAudioUnit.cpp)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    target_link_libraries(LibMedia PRIVATE ${AUDIO_TOOLBOX})
elseif(LADYBIRD_AUDIO_BACKEND STREQUAL "WASAPI")
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamWasapi.cpp)
    target_link_libraries(LibMedia PRIVATE ole32 avrt winmm)
elseif (DEFINED LADYBIRD_AUDIO_BACKEND)
    message(FATAL_ERROR "Please update ${CMAKE_CURRENT_LIST_FILE} for audio backend ${LADYBIRD_AUDIO_BACKEND}")
else ()
    target_sources(LibMedia PRIVATE Audio/PlaybackStream.cpp)
endif()
