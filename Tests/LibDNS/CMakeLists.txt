set(TEST_SOURCES
    TestDNSMessage.cpp
    TestDNSResolver.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDNS LIBS LibDNS)
endforeach()
