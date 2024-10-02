include(ffmpeg)
include(pulseaudio)

set(SOURCES
    Audio/Loader.cpp
    Audio/PlaybackStream.cpp
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
    # FIXME: Need to figure out how to build or replace ffmpeg libs on Android
    target_sources(LibMedia PRIVATE FFmpeg/FFmpegVideoDecoderStub.cpp)
endif()

# Audio backend -- how we output audio to the speakers
if (HAVE_PULSEAUDIO)
    target_sources(LibMedia PRIVATE
        Audio/PlaybackStreamPulseAudio.cpp
        Audio/PulseAudioWrappers.cpp
    )
    target_link_libraries(LibMedia PRIVATE PkgConfig::PULSEAUDIO)
    target_compile_definitions(LibMedia PUBLIC HAVE_PULSEAUDIO=1)
elseif (APPLE AND NOT IOS)
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamAudioUnit.cpp)

    find_library(AUDIO_UNIT AudioUnit REQUIRED)
    target_link_libraries(LibMedia PRIVATE ${AUDIO_UNIT})
elseif (ANDROID)
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamOboe.cpp)
    find_package(oboe REQUIRED CONFIG)
    target_link_libraries(LibMedia PRIVATE log oboe::oboe)
else()
    message(WARNING "No audio backend available")
endif()
