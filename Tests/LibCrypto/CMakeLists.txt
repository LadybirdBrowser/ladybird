set(TEST_SOURCES
    TestAES.cpp
    TestArgon2.cpp
    TestASN1.cpp
    TestBigFraction.cpp
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

ladybird_test_suite(TestCrypto LibCrypto SOURCES ${TEST_SOURCES} LIBS LibCrypto)
