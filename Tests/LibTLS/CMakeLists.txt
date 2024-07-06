set(TEST_SOURCES
    TestTLSCertificateParser.cpp
    TestTLSHandshake.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibTLS LIBS LibTLS LibCrypto)
endforeach()
