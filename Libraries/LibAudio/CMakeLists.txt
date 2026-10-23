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
    ../../Services/AudioServer/AudioServerControlClientEndpoint.h
    ../../Services/AudioServer/AudioServerControlEndpoint.h
)

if (APPLE)
    list(APPEND SOURCES CoreAudioChannelLayout.cpp)
endif()

if (WIN32)
    list(APPEND SOURCES WindowsChannelMask.cpp)
endif()

compile_ipc(AudioClient.ipc AudioClientEndpoint.h)
compile_ipc(AudioServer.ipc AudioServerEndpoint.h)
compile_ipc(../../Services/AudioServer/AudioServerControl.ipc ../../Services/AudioServer/AudioServerControlEndpoint.h)
compile_ipc(../../Services/AudioServer/AudioServerControlClient.ipc ../../Services/AudioServer/AudioServerControlClientEndpoint.h)

ladybird_lib(LibAudio audio EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibAudio PRIVATE LibCore LibIPC LibThreading)

if (APPLE)
    target_link_libraries(LibAudio PRIVATE "-framework AudioToolbox")
endif()
