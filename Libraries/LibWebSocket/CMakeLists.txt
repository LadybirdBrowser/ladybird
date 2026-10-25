set(SOURCES
    ConnectionInfo.cpp
    Impl/AcceptedSocket.cpp
    Impl/WebSocketImpl.cpp
    WebSocket.cpp
)

ladybird_lib(LibWebSocket websocket)
target_link_libraries(LibWebSocket PUBLIC LibHTTP)
target_link_libraries(LibWebSocket PRIVATE LibCore LibCrypto LibURL LibDNS)
