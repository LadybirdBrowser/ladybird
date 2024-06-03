set(WEBWORKER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/WebWorker)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(WEBWORKER_SOURCES
    "${WEBWORKER_SOURCE_DIR}/ConnectionFromClient.cpp"
    "${WEBWORKER_SOURCE_DIR}/DedicatedWorkerHost.cpp"
    "${WEBWORKER_SOURCE_DIR}/PageHost.cpp"
    ../HelperProcess.cpp
    ../Utilities.cpp
)

# FIXME: Add Android service

add_library(webworker STATIC ${WEBWORKER_SOURCES})

target_include_directories(webworker PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(webworker PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_include_directories(webworker PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(webworker PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibProtocol LibWeb LibWebView LibLocale LibImageDecoderClient LibMain LibSQL LibURL)

add_executable(WebWorker main.cpp)
target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_link_libraries(WebWorker PRIVATE webworker)
