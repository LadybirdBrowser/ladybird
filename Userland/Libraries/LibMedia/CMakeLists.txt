include(ffmpeg)

set(SOURCES
    Audio/GenericTypes.cpp
    Audio/SampleFormats.cpp
    Audio/Loader.cpp
    Audio/WavLoader.cpp
    Audio/FlacLoader.cpp
    Audio/Metadata.cpp
    Audio/MP3Loader.cpp
    Audio/PlaybackStream.cpp
    Audio/QOALoader.cpp
    Audio/QOATypes.cpp
    Audio/VorbisComment.cpp
    Color/ColorConverter.cpp
    Color/ColorPrimaries.cpp
    Color/TransferCharacteristics.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    PlaybackManager.cpp
    VideoFrame.cpp
)

if (HAVE_PULSEAUDIO)
    list(APPEND SOURCES
        Audio/PlaybackStreamPulseAudio.cpp
        Audio/PulseAudioWrappers.cpp
    )
endif()

if (HAS_FFMPEG)
    list(APPEND SOURCES
        Audio/FFmpegLoader.cpp
        FFmpeg/FFmpegVideoDecoder.cpp
    )
else()
    list(APPEND SOURCES FFmpeg/FFmpegVideoDecoderStub.cpp)
endif()

serenity_lib(LibMedia media)
target_link_libraries(LibMedia PRIVATE LibCore LibCrypto LibRIFF LibIPC LibGfx LibThreading LibUnicode)

if (HAS_FFMPEG)
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC PkgConfig::AVFORMAT PkgConfig::AVUTIL)
endif()

if (HAVE_PULSEAUDIO)
    target_link_libraries(LibMedia PRIVATE pulse)
    target_compile_definitions(LibMedia PRIVATE HAVE_PULSEAUDIO=1)
endif()

if (APPLE AND NOT IOS)
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamAudioUnit.cpp)

    find_library(AUDIO_UNIT AudioUnit REQUIRED)
    target_link_libraries(LibMedia PRIVATE ${AUDIO_UNIT})
endif()

if (ANDROID)
    target_sources(LibMedia PRIVATE Audio/PlaybackStreamOboe.cpp)
    find_package(oboe REQUIRED CONFIG)
    target_link_libraries(LibMedia PRIVATE log oboe::oboe)
endif()
