set(REQUESTSERVER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/RequestServer)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(REQUESTSERVER_SOURCES
    ${REQUESTSERVER_SOURCE_DIR}/ConnectionFromClient.cpp
    ${REQUESTSERVER_SOURCE_DIR}/ConnectionCache.cpp
    ${REQUESTSERVER_SOURCE_DIR}/Request.cpp
    ${REQUESTSERVER_SOURCE_DIR}/HttpRequest.cpp
    ${REQUESTSERVER_SOURCE_DIR}/HttpProtocol.cpp
    ${REQUESTSERVER_SOURCE_DIR}/HttpsRequest.cpp
    ${REQUESTSERVER_SOURCE_DIR}/HttpsProtocol.cpp
    ${REQUESTSERVER_SOURCE_DIR}/Protocol.cpp
)

if (ANDROID)
    add_library(requestserver SHARED
        ${REQUESTSERVER_SOURCES}
        ../Android/src/main/cpp/RequestServerService.cpp
        ../Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ../Utilities.cpp
    )
else()
    add_library(requestserver STATIC ${REQUESTSERVER_SOURCES})
endif()

add_executable(RequestServer main.cpp)
target_link_libraries(RequestServer PRIVATE requestserver)

target_include_directories(requestserver PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(requestserver PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(requestserver PUBLIC LibCore LibMain LibCrypto LibFileSystem LibHTTP LibIPC LibMain LibTLS LibWebView LibWebSocket LibURL LibThreading)
if (${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    # Solaris has socket and networking related functions in two extra libraries
    target_link_libraries(requestserver PUBLIC nsl socket)
endif()
if (HAIKU)
    # Haiku has networking related functions in the network library
    target_link_libraries(RequestServer PRIVATE network)
endif()
