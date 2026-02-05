set(TEST_SOURCES
    TestTLSCertificateParser.cpp
    TestTLSHandshake.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibTLS LIBS LibTLS LibCrypto WORKING_DIRECTORY ${Lagom_BINARY_DIR})
endforeach()
