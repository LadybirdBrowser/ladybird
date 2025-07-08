set(TEST_SOURCES
    TestTLSCertificateParser.cpp
)

if (NOT WIN32 OR NOT ENABLE_WINDOWS_CI)
    # FIXME: This test cannot find the default OpenSSL CA certificates on Windows CI
    # https://github.com/LadybirdBrowser/ladybird/issues/5355
    list(APPEND TEST_SOURCES
       TestTLSHandshake.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibTLS LIBS LibTLS LibCrypto WORKING_DIRECTORY ${Lagom_BINARY_DIR})
endforeach()
