set(SOURCES
    Client.cpp
    WebAudioClient.cpp
)

set(GENERATED_SOURCES
    ../../Services/WebAudioWorker/WebAudioWorkerClientEndpoint.h
    ../../Services/WebAudioWorker/WebAudioWorkerServerEndpoint.h

    ../../Services/AudioServer/WebAudioClientEndpoint.h
    ../../Services/AudioServer/WebAudioServerEndpoint.h
)

ladybird_lib(LibWebAudioWorkerClient webaudiorendererclient)
target_link_libraries(LibWebAudioWorkerClient PRIVATE LibCore LibIPC)
