set(TEST_SOURCES
    TestDNSResolver.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibDNS LIBS LibDNS LibTLS)
endforeach()
