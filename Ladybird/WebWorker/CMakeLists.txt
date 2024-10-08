include(fontconfig)

set(WEBWORKER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/WebWorker)

set(WEBWORKER_SOURCES
    "${WEBWORKER_SOURCE_DIR}/ConnectionFromClient.cpp"
    "${WEBWORKER_SOURCE_DIR}/DedicatedWorkerHost.cpp"
    "${WEBWORKER_SOURCE_DIR}/PageHost.cpp"
    ../FontPlugin.cpp
    ../HelperProcess.cpp
    ../Utilities.cpp
)

# FIXME: Add Android service

add_library(webworkerservice STATIC ${WEBWORKER_SOURCES})
set_target_properties(webworkerservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(webworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_include_directories(webworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibRequests LibWeb LibWebView LibUnicode LibImageDecoderClient LibMain LibURL)

if (HAS_FONTCONFIG)
    target_link_libraries(webworkerservice PRIVATE Fontconfig::Fontconfig)
endif()

if (ENABLE_QT)
    qt_add_executable(WebWorker
            ../Qt/EventLoopImplementationQt.cpp
            ../Qt/EventLoopImplementationQtEventTarget.cpp
            ../Qt/StringUtils.cpp
            main.cpp
    )
    target_link_libraries(WebWorker PRIVATE Qt::Core)
    target_link_libraries(WebWorker PRIVATE webworkerservice LibWebSocket)
    target_compile_definitions(WebWorker PRIVATE HAVE_QT=1)

else()
    add_executable(WebWorker main.cpp)
endif()

target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_link_libraries(WebWorker PRIVATE webworkerservice)
