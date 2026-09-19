set(TEST_SOURCES
    TestDNSMessage.cpp
    TestDNSResolver.cpp
    TestDNSSEC.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDNS LIBS LibDNS LibCrypto)
endforeach()
