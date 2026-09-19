ladybird_test(TestAIA.cpp RequestServer LIBS OpenSSL::Crypto OpenSSL::SSL CURL::libcurl)
target_sources(TestAIA PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/RequestServer/AIA.cpp)
target_include_directories(TestAIA PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

ladybird_test(TestContentCodingRetry.cpp RequestServer LIBS requestserverservice)
target_include_directories(TestContentCodingRetry PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

ladybird_test(TestCookieResponses.cpp RequestServer LIBS requestserverservice)
target_include_directories(TestCookieResponses PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

ladybird_test(TestDiskCacheWaiters.cpp RequestServer LIBS requestserverservice)
target_include_directories(TestDiskCacheWaiters PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

ladybird_test(TestRequestTimingInfo.cpp RequestServer LIBS requestserverservice)
target_include_directories(TestRequestTimingInfo PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

if (NOT WIN32)
    ladybird_test(TestTransportPlatform.cpp RequestServer NAME TestRequestServerTransportPlatform LIBS requestserverservice)
    target_include_directories(TestRequestServerTransportPlatform PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
endif()

if (LINUX)
    ladybird_test(TestResourceMapSandbox.cpp RequestServer LIBS requestserverservice)
    target_include_directories(TestResourceMapSandbox PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
endif()
