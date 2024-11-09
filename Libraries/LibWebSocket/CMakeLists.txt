set(SOURCES
    ConnectionInfo.cpp
    Impl/WebSocketImpl.cpp
    Impl/WebSocketImplSerenity.cpp
    WebSocket.cpp
)

serenity_lib(LibWebSocket websocket)
target_link_libraries(LibWebSocket PRIVATE LibCore LibCrypto LibTLS LibURL)
