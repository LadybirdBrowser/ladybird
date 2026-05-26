set(SOURCES
    BackingStoreManager.cpp
    CompositorState.cpp
    ConnectionFromClient.cpp
    ConnectionFromWebContent.cpp
)

set(GENERATED_SOURCES
    CompositorControlClientEndpoint.h
    CompositorControlServerEndpoint.h
    CompositorWebContentClientEndpoint.h
    CompositorWebContentServerEndpoint.h
)

add_library(compositorservice STATIC ${SOURCES} ${GENERATED_SOURCES})
ladybird_generated_sources(compositorservice)

add_executable(Compositor main.cpp)

target_include_directories(compositorservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(compositorservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(Compositor PRIVATE compositorservice LibCore LibMain LibWebView)
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibIPC LibMedia LibSync LibWeb)

if (WIN32)
    target_include_directories(Compositor PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_include_directories(compositorservice PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(Compositor CONSOLE)
endif()
