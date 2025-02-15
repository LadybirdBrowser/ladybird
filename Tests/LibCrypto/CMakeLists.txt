set(TEST_SOURCES
    TestAES.cpp
    TestASN1.cpp
    TestBigFraction.cpp
    TestBigInteger.cpp
    TestChecksum.cpp
    TestCurves.cpp
    TestEd25519.cpp
    TestHash.cpp
    TestHKDF.cpp
    TestHMAC.cpp
    TestMGF.cpp
    TestOAEP.cpp
    TestPBKDF2.cpp
    TestPSS.cpp
    TestRSA.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibCrypto LIBS LibCrypto)
endforeach()
