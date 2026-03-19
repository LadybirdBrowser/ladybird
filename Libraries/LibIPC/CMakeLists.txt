set(SOURCES
    AutoCloseFileDescriptor.cpp
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    TransportHandle.cpp
)

if (APPLE AND NOT IOS)
    list(APPEND SOURCES
        AttachmentMachPort.cpp
        File.cpp
        Message.cpp
        TransportBootstrapMach.cpp
        TransportMachPort.cpp)
elseif (UNIX)
    list(APPEND SOURCES
        Attachment.cpp
        File.cpp
        Message.cpp
        TransportSocket.cpp)
else()
    list(APPEND SOURCES
        Attachment.cpp
        FileWindows.cpp
        MessageWindows.cpp
        TransportSocketWindows.cpp)
endif()

ladybird_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibURL LibThreading)
