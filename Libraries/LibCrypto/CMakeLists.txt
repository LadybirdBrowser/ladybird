add_cxx_compile_options(-Wvla)

set(SOURCES
    OpenSSL.cpp
    ASN1/ASN1.cpp
    ASN1/DER.cpp
    ASN1/PEM.cpp
    Authentication/HMAC.cpp
    BigFraction/BigFraction.cpp
    BigInt/Algorithms/BitwiseOperations.cpp
    BigInt/Algorithms/Division.cpp
    BigInt/Algorithms/GCD.cpp
    BigInt/Algorithms/ModularInverse.cpp
    BigInt/Algorithms/ModularPower.cpp
    BigInt/Algorithms/Multiplication.cpp
    BigInt/Algorithms/SimpleOperations.cpp
    BigInt/SignedBigInteger.cpp
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
    NumberTheory/ModularFunctions.cpp
    PK/RSA.cpp
    PK/EC.cpp
    SecureRandom.cpp
)

serenity_lib(LibCrypto crypto)
target_link_libraries(LibCrypto PRIVATE LibCore)

find_package(OpenSSL REQUIRED)
target_link_libraries(LibCrypto PUBLIC OpenSSL::Crypto)
