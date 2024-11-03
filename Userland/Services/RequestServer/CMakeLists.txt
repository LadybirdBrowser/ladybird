compile_ipc(RequestServer.ipc RequestServerEndpoint.h)
compile_ipc(RequestClient.ipc RequestClientEndpoint.h)

set(SOURCES
    ConnectionFromClient.cpp
    Request.cpp
    main.cpp
)

set(GENERATED_SOURCES
    RequestClientEndpoint.h
    RequestServerEndpoint.h
)

serenity_bin(RequestServer)
target_link_libraries(RequestServer PRIVATE LibCore LibCrypto LibIPC LibMain LibTLS LibWebSocket LibURL LibThreading)
