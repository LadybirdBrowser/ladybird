ladybird_test_suite(LibURL)

set(URL_TEST_SOURCES
    TestPublicSuffix.cpp
    TestURL.cpp
    TestURLPattern.cpp
)

target_sources(TestLibURL PRIVATE ${URL_TEST_SOURCES})
target_link_libraries(TestLibURL PRIVATE LibURL LibRegex)
