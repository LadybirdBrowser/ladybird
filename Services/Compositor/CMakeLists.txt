set(SOURCES
    BackingStoreManager.cpp
    CompositorState.cpp
    ContextState.cpp
    ConnectionFromClient.cpp
    ConnectionFromWebContent.cpp
    VSyncScheduler.cpp
    ViewportScrollbarController.cpp
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

if (LINUX)
    target_sources(Compositor PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(Compositor PRIVATE SandboxMacOS.cpp)
else()
    target_sources(Compositor PRIVATE SandboxUnimplemented.cpp)
endif()

target_include_directories(compositorservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(compositorservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(Compositor PRIVATE compositorservice LibCore LibMain LibSandbox LibWebView)
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibIPC LibMedia LibSync LibWeb)

if (APPLE)
    target_link_libraries(Compositor PRIVATE "-framework CoreGraphics" "-framework CoreVideo")
endif()

if (WIN32)
    target_include_directories(Compositor PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_include_directories(compositorservice PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(Compositor CONSOLE)
endif()
