include(audio)

if (NOT ANDROID)
    include(ffmpeg)
endif()

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

serenity_lib(LibMedia media)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibRIFF LibIPC LibGfx LibThreading LibUnicode)

if (NOT ANDROID)
    target_sources(LibMedia PRIVATE
            Audio/FFmpegLoader.cpp
            FFmpeg/FFmpegVideoDecoder.cpp
    )
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC PkgConfig::AVFORMAT PkgConfig::AVUTIL)
else()
    # FIXME: Need to figure out how to build or replace ffmpeg libs on Android and Windows
    target_sources(LibMedia PRIVATE FFmpeg/FFmpegVideoDecoderStub.cpp)
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
