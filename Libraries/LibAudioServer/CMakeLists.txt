compile_ipc(ToSessionClientFromAudioServer.ipc ../../Libraries/LibAudioServer/ToSessionClientFromAudioServerEndpoint.h)
compile_ipc(ToBrokerFromAudioServer.ipc ../../Libraries/LibAudioServer/ToBrokerFromAudioServerEndpoint.h)
compile_ipc(ToAudioServerFromBroker.ipc ../../Libraries/LibAudioServer/ToAudioServerFromBrokerEndpoint.h)
compile_ipc(ToAudioServerFromSessionClient.ipc ../../Libraries/LibAudioServer/ToAudioServerFromSessionClientEndpoint.h)

set(SOURCES
    BrokerOfAudioServer.cpp
    SessionClientOfAudioServer.cpp
    SingleSinkSessionClient.cpp
    SharedCircularBuffer.cpp
    LibAudioServer.cpp
)

set(GENERATED_SOURCES
    ../../Libraries/LibAudioServer/ToBrokerFromAudioServerEndpoint.h
    ../../Libraries/LibAudioServer/ToAudioServerFromBrokerEndpoint.h
    ../../Libraries/LibAudioServer/ToSessionClientFromAudioServerEndpoint.h
    ../../Libraries/LibAudioServer/ToAudioServerFromSessionClientEndpoint.h
)

ladybird_lib(LibAudioServer audioserver)
target_link_libraries(LibAudioServer PRIVATE LibCore LibIPC)
