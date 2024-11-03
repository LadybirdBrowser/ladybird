set(REQUESTSERVER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/RequestServer)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(REQUESTSERVER_SOURCES
    ${REQUESTSERVER_SOURCE_DIR}/ConnectionFromClient.cpp
)

if (ANDROID)
    add_library(requestserverservice SHARED
        ${REQUESTSERVER_SOURCES}
        ../Android/src/main/cpp/RequestServerService.cpp
        ../Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ../Utilities.cpp
    )
else()
    add_library(requestserverservice STATIC ${REQUESTSERVER_SOURCES})
endif()

find_package(PkgConfig)
find_package(CURL REQUIRED)

add_executable(RequestServer main.cpp)
target_link_libraries(RequestServer PRIVATE requestserverservice)

target_include_directories(requestserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(requestserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(requestserverservice PUBLIC LibCore LibMain LibCrypto LibFileSystem LibIPC LibMain LibTLS LibWebView LibWebSocket LibURL LibTextCodec LibThreading CURL::libcurl)
if (${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    # Solaris has socket and networking related functions in two extra libraries
    target_link_libraries(requestserverservice PUBLIC nsl socket)
endif()
if (HAIKU)
    # Haiku has networking related functions in the network library
    target_link_libraries(RequestServer PRIVATE network)
endif()
