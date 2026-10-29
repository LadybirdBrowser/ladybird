ladybird_test_suite(LibDNS)

set(TEST_SOURCES
    TestDNSMessage.cpp
    TestDNSResolver.cpp
    TestDNSSEC.cpp
)

target_sources(TestLibDNS PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibDNS PRIVATE LibDNS LibCrypto)
