set(SOURCES
    Client.cpp
)

if (NOT SERENITYOS)
    compile_ipc(../../Services/WebRTCClient/WebRTCClientClient.ipc ../../Services/WebRTCClient/WebRTCClientClientEndpoint.h)
    compile_ipc(../../Services/WebRTCClient/WebRTCClientServer.ipc ../../Services/WebRTCClient/WebRTCClientServerEndpoint.h)
endif()

set(GENERATED_SOURCES
    ../../Services/WebRTCClient/WebRTCClientClientEndpoint.h
    ../../Services/WebRTCClient/WebRTCClientServerEndpoint.h
)

ladybird_lib(LibWebRTCClient webrtcclient)
target_link_libraries(LibWebRTCClient PRIVATE LibCore LibIPC)
