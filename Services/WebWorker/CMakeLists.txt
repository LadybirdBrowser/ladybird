set(WEBWORKER_SOURCES
    ConnectionFromClient.cpp
    DedicatedWorkerHost.cpp
    PageHost.cpp
)

# FIXME: Add Android service

add_library(webworkerservice STATIC ${WEBWORKER_SOURCES})

target_include_directories(webworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibRequests LibWeb LibWebView LibUnicode LibImageDecoderClient LibMain LibURL LibGC)

add_executable(WebWorker main.cpp)
target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(WebWorker PRIVATE webworkerservice)
