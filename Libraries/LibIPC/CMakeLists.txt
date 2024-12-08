set(SOURCES
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
)

if (UNIX)
    list(APPEND SOURCES
        File.cpp
        Message.cpp
        TransportSocket.cpp)
else()
    list(APPEND SOURCES
        FileWindows.cpp
        MessageWindows.cpp
        TransportSocketWindows.cpp)
endif()

serenity_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibURL LibThreading)
