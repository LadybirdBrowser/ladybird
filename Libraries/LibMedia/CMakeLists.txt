include(audio)

include(ffmpeg)

set(SOURCES
    Audio/Loader.cpp
    Audio/SampleFormats.cpp
    Color/ColorConverter.cpp
    Color/ColorPrimaries.cpp
    Color/TransferCharacteristics.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    PlaybackManager.cpp
    VideoFrame.cpp
)

ladybird_lib(LibMedia media EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibIPC LibGfx LibThreading LibUnicode)

target_sources(LibMedia PRIVATE
        Audio/FFmpegLoader.cpp
        FFmpeg/FFmpegDemuxer.cpp
        FFmpeg/FFmpegIOContext.cpp
        FFmpeg/FFmpegVideoDecoder.cpp
)

if (NOT ANDROID)
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC PkgConfig::AVFORMAT PkgConfig::AVUTIL)
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
    find_library(AUDIO_UNIT AudioUnit REQUIRED)
    target_link_libraries(LibMedia PRIVATE ${AUDIO_UNIT})
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "OBOE")
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamOboe.cpp)
    find_package(oboe REQUIRED CONFIG)
    target_link_libraries(LibMedia PRIVATE log oboe::oboe)
elseif (DEFINED LADYBIRD_AUDIO_BACKEND)
    message(FATAL_ERROR "Please update ${CMAKE_CURRENT_LIST_FILE} for audio backend ${LADYBIRD_AUDIO_BACKEND}")
else ()
    target_sources(LibMedia PRIVATE Audio/PlaybackStream.cpp)
endif()
