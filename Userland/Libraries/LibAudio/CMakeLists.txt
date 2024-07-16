set(SOURCES
    GenericTypes.cpp
    SampleFormats.cpp
    Loader.cpp
    WavLoader.cpp
    FlacLoader.cpp
    FlacWriter.cpp
    WavWriter.cpp
    Metadata.cpp
    MP3Loader.cpp
    PlaybackStream.cpp
    QOALoader.cpp
    QOATypes.cpp
    UserSampleQueue.cpp
    VorbisComment.cpp
)

if (HAVE_PULSEAUDIO)
    list(APPEND SOURCES
        PlaybackStreamPulseAudio.cpp
        PulseAudioWrappers.cpp
    )
endif()

serenity_lib(LibAudio audio)
target_link_libraries(LibAudio PRIVATE LibCore LibRIFF LibIPC LibThreading LibUnicode LibCrypto)

if (HAVE_PULSEAUDIO)
    target_link_libraries(LibAudio PRIVATE pulse)
    target_compile_definitions(LibAudio PRIVATE HAVE_PULSEAUDIO=1)
endif()

if (APPLE AND NOT IOS)
    target_sources(LibAudio PRIVATE PlaybackStreamAudioUnit.cpp)

    find_library(AUDIO_UNIT AudioUnit REQUIRED)
    target_link_libraries(LibAudio PRIVATE ${AUDIO_UNIT})
endif()

if (ANDROID)
    target_sources(LibAudio PRIVATE PlaybackStreamOboe.cpp)
    find_package(oboe REQUIRED CONFIG)
    target_link_libraries(LibAudio PRIVATE log oboe::oboe)
endif()
