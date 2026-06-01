set(TEST_SOURCES
    TestTLSCertificateParser.cpp
    TestTLSHandshake.cpp
)

ladybird_test_suite(TestTLS LibTLS SOURCES ${TEST_SOURCES} LIBS LibTLS LibCrypto)
