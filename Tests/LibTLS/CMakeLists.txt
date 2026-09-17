ladybird_test_suite(LibTLS)

set(TEST_SOURCES
    TestTLSCertificateParser.cpp
    TestTLSHandshake.cpp
    TestTLSHostname.cpp
)

target_sources(TestLibTLS PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibTLS PRIVATE LibTLS LibCrypto LibFileSystem LibThreading)
