add_compile_options(-Wvla)

set(SOURCES
    Certificate.cpp
    Handshake.cpp
    HandshakeCertificate.cpp
    HandshakeClient.cpp
    HandshakeServer.cpp
    Record.cpp
    Socket.cpp
    TLSv12.cpp
)

serenity_lib(LibTLS tls)
target_link_libraries(LibTLS PRIVATE LibCore LibCrypto LibFileSystem)

include(ca_certificates_data)
