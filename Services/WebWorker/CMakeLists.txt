set(WEBWORKER_SOURCES
    ConnectionFromClient.cpp
    PageHost.cpp
    WebWorkerCompositorHost.cpp
    WorkerHost.cpp
    WorkerThread.cpp
)

# FIXME: Add Android service

add_library(webworkerservice STATIC ${WEBWORKER_SOURCES})
set_target_properties(webworkerservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(webworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibHTTP LibIPC LibJS LibRequests LibCompositing LibWeb LibUnicode LibImageDecoderClient LibMain LibMediaClient LibThreading LibURL LibGC)
target_link_libraries(webworkerservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)

