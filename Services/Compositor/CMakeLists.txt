set(SOURCES
    BackingStoreManager.cpp
    CanvasHost.cpp
    CompositorState.cpp
    ContextState.cpp
    ConnectionFromClient.cpp
    ConnectionFromWebContent.cpp
    HostWebGLContext.cpp
    OpenGLContext.cpp
    VSyncScheduler.cpp
    ViewportScrollbarController.cpp
    WebGLObjectMap.cpp
)

invoke_py_generator(
    "WebGLCommandReplayer.cpp"
    "generate_compositor_webgl_replayer.py"
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/WebGL/GLFunctions.json"
    "WebGLCommandReplayer.h"
    "WebGLCommandReplayer.cpp"
    arguments -j "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/WebGL/GLFunctions.json"
    dependencies "${LADYBIRD_SOURCE_DIR}/Meta/Generators/libweb_webgl.py"
)

set(GENERATED_SOURCES
    CompositorControlClientEndpoint.h
    CompositorControlServerEndpoint.h
    CompositorWebContentClientEndpoint.h
    CompositorWebContentServerEndpoint.h
    ${CMAKE_BINARY_DIR}/Libraries/LibWeb/WebGL/GLFunctions.cpp
    WebGLCommandReplayer.cpp
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
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibIPC LibMedia LibSync LibWeb ${ANGLE_TARGETS})

if (APPLE)
    target_link_libraries(Compositor PRIVATE "-framework CoreGraphics" "-framework CoreVideo")
endif()

if (WIN32)
    target_include_directories(Compositor PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_include_directories(compositorservice PRIVATE $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    ladybird_windows_bin(Compositor CONSOLE)
endif()
