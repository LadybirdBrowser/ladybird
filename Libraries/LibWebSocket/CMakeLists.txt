set(SOURCES
    ConnectionInfo.cpp
    Impl/WebSocketImpl.cpp
    WebSocket.cpp
)

ladybird_lib(LibWebSocket websocket)
target_link_libraries(LibWebSocket PRIVATE LibCore LibCrypto LibHTTP LibURL LibDNS)
