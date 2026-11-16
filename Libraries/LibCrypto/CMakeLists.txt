add_cxx_compile_options(-Wvla)

set(SOURCES
    KeyHandle.cpp
    OpenSSL.cpp
    RustBindings.cpp
    ASN1/ASN1.cpp
    ASN1/DER.cpp
    Authentication/HMAC.cpp
    Authentication/KMAC.cpp
    BigInt/SignedBigInteger.cpp
    BigInt/UnsignedBigInteger.cpp
    Certificate/Certificate.cpp
    Cipher/AES.cpp
    Cipher/ChaCha.cpp
    Curves/EdwardsCurve.cpp
    Curves/SECPxxxr1.cpp
    Hash/Argon2.cpp
    Hash/Hash.cpp
    Hash/SHAKE.cpp
    Hash/HKDF.cpp
    Hash/PBKDF2.cpp
    PK/EC.cpp
    PK/MLDSA.cpp
    PK/MLKEM.cpp
    PK/ModuleLatticePrivateKey.cpp
    PK/RSA.cpp
)

ladybird_lib(LibCrypto crypto)

target_link_libraries(LibCrypto PRIVATE PkgConfig::libtommath)
target_link_libraries(LibCrypto PUBLIC OpenSSL::Crypto)
