set(URL_TEST_SOURCES
    TestURL.cpp
    TestURLPattern.cpp
    TestPublicSuffix.cpp
)

ladybird_test_suite(TestURL LibURL SOURCES ${URL_TEST_SOURCES} LIBS LibURL LibRegex)
