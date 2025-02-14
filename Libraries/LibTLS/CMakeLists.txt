add_cxx_compile_options(-Wvla)

set(SOURCES
    DefaultRootCACertificates.cpp
    TLSv12.cpp
)

serenity_lib(LibTLS tls)
target_link_libraries(LibTLS PRIVATE LibCore LibCrypto LibFileSystem)

include(ca_certificates_data)

find_package(OpenSSL REQUIRED)
target_link_libraries(LibTLS PUBLIC OpenSSL::SSL)
