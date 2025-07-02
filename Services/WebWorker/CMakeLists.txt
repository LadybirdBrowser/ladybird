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

target_link_libraries(webworkerservice PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibRequests LibWeb LibWebView LibUnicode LibImageDecoderClient LibMain LibURL LibGC)
target_link_libraries(webworkerservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)

add_executable(WebWorker main.cpp)

target_include_directories(WebWorker PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(WebWorker PRIVATE webworkerservice)

if(WIN32)
    # FIXME: This is a hack to get around lld-link error undefined symbols in LibWeb
    target_link_libraries(webworkerservice PRIVATE
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,CallbackType>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,DedicatedWorkerGlobalScope>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,EnvironmentSettingsSnapshot>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,EventNames>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,SharedWorkerGlobalScope>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,Tracing>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,WorkerDebugConsoleClient>
            $<FILTER:$<TARGET_OBJECTS:LibWeb>,INCLUDE,WorkerLocation>
    )
    find_package(pthread REQUIRED)
    target_include_directories(WebWorker PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
endif()
