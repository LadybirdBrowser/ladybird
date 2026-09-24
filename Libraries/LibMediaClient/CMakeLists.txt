set(SOURCES
    Client.cpp
)

compile_ipc(../../Services/MediaServer/MediaClient.ipc ../../Services/MediaServer/MediaClientEndpoint.h)
compile_ipc(../../Services/MediaServer/MediaServer.ipc ../../Services/MediaServer/MediaServerEndpoint.h)

set(GENERATED_SOURCES
    ../../Services/MediaServer/MediaClientEndpoint.h
    ../../Services/MediaServer/MediaServerEndpoint.h
)

ladybird_lib(LibMediaClient mediaclient)
target_link_libraries(LibMediaClient PRIVATE LibCore LibIPC)
