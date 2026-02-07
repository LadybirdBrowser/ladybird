set(SOURCES
    CacheSizes.cpp
    NetworkError.h
    Request.cpp
    RequestClient.cpp
    RequestTimingInfo.cpp
    WebSocket.cpp
)

set(GENERATED_SOURCES
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
)

ladybird_lib(LibRequests requests)
target_link_libraries(LibRequests PRIVATE LibCore LibHTTP LibIPC LibURL)
