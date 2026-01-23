set(SOURCES
    Client.cpp
)

if (NOT SERENITYOS)
    compile_ipc(../../Services/MediaServer/MediaServerClient.ipc ../../Services/MediaServer/MediaServerClientEndpoint.h)
    compile_ipc(../../Services/MediaServer/MediaServerServer.ipc ../../Services/MediaServer/MediaServerServerEndpoint.h)
endif()

set(GENERATED_SOURCES
    ../../Services/MediaServer/MediaServerClientEndpoint.h
    ../../Services/MediaServer/MediaServerServerEndpoint.h
)

ladybird_lib(LibMediaServerClient mediaserverclient)
target_link_libraries(LibMediaServerClient PRIVATE LibCore LibIPC)
