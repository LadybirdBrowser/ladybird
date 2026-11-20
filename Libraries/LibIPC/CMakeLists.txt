set(SOURCES
    AutoCloseFileDescriptor.cpp
    Connection.cpp
    Decoder.cpp
    Encoder.cpp
    File.cpp
    Message.cpp
    RawAttachment.cpp
    RawMessage.cpp
    ReceivedMessageBytes.cpp
    StartupTransport.cpp
    ThreadSafeMessagePoster.cpp
    TransportHandle.cpp
)

if (APPLE)
    # The Mach transport is the Rust one, which LibIPC's copy of the crate gives C++ an interface to.
    import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libipc_rust FEATURES allocator cpp_transport FFI_HEADER RustFFI.h)
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
        HandleBroker.cpp
        HandleBrokerServer.cpp
        RustTransportWindows.cpp
        TransportWindows.cpp)
endif()

ladybird_lib(LibIPC ipc)
target_link_libraries(LibIPC PRIVATE LibCore LibThreading LibURL)
if (APPLE)
    target_link_libraries(LibIPC PRIVATE libipc_rust)
endif()
