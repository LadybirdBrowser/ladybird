ladybird_test_suite(LibCrypto)

set(TEST_SOURCES
    TestAES.cpp
    TestArgon2.cpp
    TestASN1.cpp
    TestBigInteger.cpp
    TestCurves.cpp
    TestEd25519.cpp
    TestEd448.cpp
    TestHash.cpp
    TestHKDF.cpp
    TestHMAC.cpp
    TestMLDSA.cpp
    TestOAEP.cpp
    TestPBKDF2.cpp
    TestPSS.cpp
    TestRSA.cpp
)

target_sources(TestLibCrypto PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCrypto PRIVATE LibCrypto)
