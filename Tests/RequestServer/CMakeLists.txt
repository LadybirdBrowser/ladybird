ladybird_test(TestAIA.cpp RequestServer LIBS OpenSSL::Crypto OpenSSL::SSL CURL::libcurl)
target_sources(TestAIA PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/RequestServer/AIA.cpp)
target_include_directories(TestAIA PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)
