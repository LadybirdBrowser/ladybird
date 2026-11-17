ladybird_test_suite(LibCrypto)

set(TEST_SOURCES
    TestEd25519.cpp
    TestHash.cpp
    TestHMAC.cpp
    TestRSA.cpp
)

target_sources(TestLibCrypto PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibCrypto PRIVATE LibCrypto)
