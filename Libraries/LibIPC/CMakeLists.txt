set(SOURCES
    AutoCloseFileDescriptor.cpp
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    File.cpp
    Message.cpp
    ReceivedMessageBytes.cpp
    TransportHandle.cpp
)

if (APPLE)
    list(APPEND SOURCES
        AttachmentMachPort.cpp
        TransportMachPort.cpp)
    if (NOT IOS)
        list(APPEND SOURCES
            MachBootstrapListener.cpp
            TransportBootstrapMach.cpp)
    endif()
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
target_link_libraries(LibIPC PRIVATE LibCore LibThreading LibURL)
