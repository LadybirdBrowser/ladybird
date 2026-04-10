set(SOURCES
    AutoCloseFileDescriptor.cpp
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    File.cpp
    Message.cpp
    TransportHandle.cpp
)

if (APPLE AND NOT IOS)
    list(APPEND SOURCES
        AttachmentMachPort.cpp
        MachBootstrapListener.cpp
        TransportBootstrapMach.cpp
        TransportMachPort.cpp)
elseif (UNIX)
    list(APPEND SOURCES
        Attachment.cpp
        TransportSocket.cpp)
else()
    list(APPEND SOURCES
        Attachment.cpp
        TransportSocketWindows.cpp)
endif()

ladybird_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibURL LibThreading)
