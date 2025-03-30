set(SOURCES
    NetworkError.h
    Request.cpp
    RequestClient.cpp
    WebSocket.cpp
)

set(GENERATED_SOURCES
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
)

serenity_lib(LibRequests requests)
target_link_libraries(LibRequests PRIVATE LibCore LibIPC)
