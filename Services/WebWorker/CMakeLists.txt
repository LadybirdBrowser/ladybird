set(WEBWORKER_SOURCES
    ConnectionFromClient.cpp
    DedicatedWorkerHost.cpp
    PageHost.cpp
)

# FIXME: Add Android service

add_library(webworkerservice STATIC ${WEBWORKER_SOURCES})
set_target_properties(webworkerservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(webworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibRequests LibWeb LibWebView LibUnicode LibImageDecoderClient LibMain LibURL LibGC)
target_link_libraries(webworkerservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)

if (ENABLE_QT)
    qt_add_executable(WebWorker main.cpp)
    target_link_libraries(WebWorker PRIVATE webworkerservice LibWebSocket)
    target_compile_definitions(WebWorker PRIVATE HAVE_QT=1)
else()
    add_executable(WebWorker main.cpp)
endif()

target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(WebWorker PRIVATE webworkerservice)
