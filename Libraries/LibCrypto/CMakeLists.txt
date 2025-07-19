add_cxx_compile_options(-Wvla)

set(SOURCES
    # reordered to avoid mp_int redefinition in unity build
    BigInt/SignedBigInteger.cpp
    OpenSSL.cpp
    ASN1/ASN1.cpp
    ASN1/DER.cpp
    ASN1/PEM.cpp
    Authentication/HMAC.cpp
    BigFraction/BigFraction.cpp
    BigInt/UnsignedBigInteger.cpp
    Certificate/Certificate.cpp
    Cipher/AES.cpp
    Curves/EdwardsCurve.cpp
    Curves/SECPxxxr1.cpp
    Hash/BLAKE2b.cpp
    Hash/HKDF.cpp
    Hash/MD5.cpp
    Hash/PBKDF2.cpp
    Hash/SHA1.cpp
    Hash/SHA2.cpp
    PK/RSA.cpp
    PK/EC.cpp
    SecureRandom.cpp
)

ladybird_lib(LibCrypto crypto)

find_package(PkgConfig REQUIRED)
pkg_check_modules(libtommath REQUIRED IMPORTED_TARGET libtommath)
target_link_libraries(LibCrypto PRIVATE PkgConfig::libtommath)

find_package(OpenSSL REQUIRED)
target_link_libraries(LibCrypto PUBLIC OpenSSL::Crypto)

if(ENABLE_UNITY_BUILD)
    set_target_properties(LibCrypto PROPERTIES UNITY_BUILD ON)
endif()
