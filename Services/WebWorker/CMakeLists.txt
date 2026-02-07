set(WEBWORKER_SOURCES
    ConnectionFromClient.cpp
    PageHost.cpp
    WorkerHost.cpp
)

# FIXME: Add Android service

add_library(webworkerservice STATIC ${WEBWORKER_SOURCES})
set_target_properties(webworkerservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(webworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibHTTP LibIPC LibJS LibRequests LibWeb LibWebView LibUnicode LibImageDecoderClient LibMain LibURL LibGC)
target_link_libraries(webworkerservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)

add_executable(WebWorker main.cpp)

target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(WebWorker PRIVATE webworkerservice OpenSSL::Crypto OpenSSL::SSL)

if(WIN32)
    find_package(pthread REQUIRED)
    target_include_directories(WebWorker PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    lagom_windows_bin(WebWorker CONSOLE)
endif()
