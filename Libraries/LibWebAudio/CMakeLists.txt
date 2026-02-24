compile_ipc(ToSessionClientFromWebAudioWorker.ipc ../../Libraries/LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h)
compile_ipc(ToWebAudioWorkerFromSessionClient.ipc ../../Libraries/LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h)
compile_ipc(ToBrokerFromWebAudioWorker.ipc ../../Libraries/LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h)
compile_ipc(ToWebAudioWorkerFromBroker.ipc ../../Libraries/LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h)

set(SOURCES
    BrokerOfWebAudioWorker.cpp
    LibWebAudio.cpp
    RingStream.cpp
    SessionClientOfWebAudioWorker.cpp
    SharedAudioBuffer.cpp
    SharedBufferStream.cpp
)

set(GENERATED_SOURCES
    ../../Libraries/LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h
    ../../Libraries/LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h
    ../../Libraries/LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h
    ../../Libraries/LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h
)

ladybird_lib(LibWebAudio webaudio)
target_link_libraries(LibWebAudio PRIVATE LibCore LibIPC LibAudioServer)
