set(SOURCES
    AudioDevices.cpp
    AudioFrameRingBase.cpp
    ClientConnection.cpp
    NullPlaybackStream.cpp
    PlaybackStream.cpp
    RecordStream.cpp
    RemotePlaybackStream.cpp
    RemoteRecordStream.cpp
    SampleSpecification.cpp
    SharedAudioFrameRing.cpp
    SpscAudioFrameRing.cpp
)

set(GENERATED_SOURCES
    AudioClientEndpoint.h
    AudioServerEndpoint.h
)

if (APPLE)
    list(APPEND SOURCES CoreAudioChannelLayout.cpp)
endif()

compile_ipc(AudioClient.ipc AudioClientEndpoint.h)
compile_ipc(AudioServer.ipc AudioServerEndpoint.h)

ladybird_lib(LibAudio audio EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibAudio PRIVATE LibCore LibIPC LibThreading)

if (APPLE)
    target_link_libraries(LibAudio PRIVATE "-framework AudioToolbox")
endif()
