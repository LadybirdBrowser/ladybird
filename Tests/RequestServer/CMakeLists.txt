ladybird_test_suite(RequestServer)

set(TEST_SOURCES
    ResourceSubstitutionMap.cpp
    TestAIA.cpp
    TestAlternativeServices.cpp
    TestContentCodingRetry.cpp
    TestCookieResponses.cpp
    TestDiskCacheWaiters.cpp
    TestExportImport.cpp
    TestRequestTimingInfo.cpp
    TestSiteBinding.cpp
    ${LADYBIRD_SOURCE_DIR}/Services/RequestServer/AIA.cpp
)

if (LINUX)
    list(APPEND TEST_SOURCES TestResourceMapSandbox.cpp)
endif()

target_sources(TestRequestServer PRIVATE ${TEST_SOURCES})
target_link_libraries(TestRequestServer PRIVATE requestserverservice OpenSSL::Crypto OpenSSL::SSL CURL::libcurl)
target_include_directories(TestRequestServer PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
