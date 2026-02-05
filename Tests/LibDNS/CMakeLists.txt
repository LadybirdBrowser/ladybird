set(TEST_SOURCES
    TestDNSResolver.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDNS LIBS LibDNS LibTLS)
endforeach()
