add_cxx_compile_options(-Wvla)

set(SOURCES
    OpenSSL.cpp
    Authentication/HMAC.cpp
    Curves/EdwardsCurve.cpp
    Hash/Hash.cpp
    PK/EC.cpp
    PK/RSA.cpp
)

ladybird_lib(LibCrypto crypto)

target_link_libraries(LibCrypto PUBLIC OpenSSL::Crypto)
