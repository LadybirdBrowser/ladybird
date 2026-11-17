ladybird_test_suite(LibCrypto)

set(TEST_SOURCES
    TestCurves.cpp
    TestEd25519.cpp
    TestEd448.cpp
    TestHash.cpp
    TestHMAC.cpp
    TestRSA.cpp
)

target_sources(TestLibCrypto PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCrypto PRIVATE LibCrypto)
