add_compile_options(-Wvla)

set(SOURCES
    Certificate.cpp
    TLSv12.cpp
)

find_package(WolfSSL REQUIRED)

serenity_lib(LibTLS tls)
target_link_libraries(LibTLS PRIVATE LibCore LibCrypto wolfssl::wolfssl)

include(ca_certificates_data)
