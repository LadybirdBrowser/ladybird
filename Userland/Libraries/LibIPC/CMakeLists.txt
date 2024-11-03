set(SOURCES
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    Message.cpp
)

if (UNIX)
    list(APPEND SOURCES TransportSocket.cpp)
endif()

serenity_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibURL LibThreading)
