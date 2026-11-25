set(SOURCES
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

if (NOT WIN32)
    # The transport is the Rust one, which LibIPC's copy of the crate gives C++ an interface to.
    import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libipc_rust FEATURES allocator cpp_transport FFI_HEADER RustFFI.h)
    list(APPEND SOURCES Transport.cpp)
endif()

if (APPLE)
    list(APPEND SOURCES AttachmentMachPort.cpp)
    if (NOT IOS)
        list(APPEND SOURCES
            MachBootstrapListener.cpp
            TransportBootstrapMach.cpp)
    endif()
elseif (UNIX)
    list(APPEND SOURCES Attachment.cpp)
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
if (NOT WIN32)
    target_link_libraries(LibIPC PRIVATE libipc_rust)
endif()
if ((LINUX OR BSD) AND BUILD_SHARED_LIBS)
    # NB: The crate is an implementation detail of this library, and other libraries carry their own copies of it. Keep
    #     its symbols, including its copy of the Rust standard library, out of the dynamic symbol table, as ELF symbol
    #     interposition would otherwise let the other copies in the process bind to this one.
    target_link_options(LibIPC PRIVATE LINKER:--exclude-libs=liblibipc_rust.a)
elseif (LINUX OR BSD)
    target_link_options(LibIPC INTERFACE LINKER:--allow-multiple-definition)
endif()
