set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    ConnectionFromClient.cpp
    WebSocketImplCurl.cpp
)

set(GENERATED_SOURCES
    RequestClientEndpoint.h
    RequestServerEndpoint.h
)

set(RS_LIB_TYPE STATIC)
if (ANDROID)
    list(APPEND SOURCES
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/RequestServerService.cpp
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
    )
    set(RS_LIB_TYPE SHARED)
endif()

add_library(requestserverservice ${RS_LIB_TYPE} ${SOURCES} ${GENERATED_SOURCES})
ladybird_generated_sources(requestserverservice)

find_package(PkgConfig)
find_package(CURL REQUIRED)

add_executable(RequestServer main.cpp)

target_include_directories(requestserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(requestserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(RequestServer PRIVATE requestserverservice)
target_link_libraries(requestserverservice PUBLIC LibCore LibDNS LibMain LibCrypto LibFileSystem LibIPC LibMain LibTLS LibWebSocket LibURL LibTextCodec LibThreading CURL::libcurl)
target_link_libraries(requestserverservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)

if (WIN32)
    lagom_windows_bin(RequestServer)
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    # Solaris has socket and networking related functions in two extra libraries
    target_link_libraries(requestserverservice PUBLIC nsl socket)
endif()
if (HAIKU)
    # Haiku has networking related functions in the network library
    target_link_libraries(RequestServer PRIVATE network)
endif()
if (ANDROID)
    # We need s_ladybird_resource_root from LibWebView
    target_link_libraries(requestserverservice PUBLIC LibWebView)
endif()
