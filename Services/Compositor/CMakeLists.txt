set(SOURCES
    AsyncScrollUpdateOutbox.cpp
    BackingStoreManager.cpp
    BitmapExport.cpp
    Canvas2DCommandPlayer.cpp
    CanvasHost.cpp
    CaretBlinkTimer.cpp
    ColorSpaceSkia.cpp
    CompositorState.cpp
    ContextPresenter.cpp
    ContextScene.cpp
    ContextState.cpp
    DisplayListPlayer.cpp
    DisplayListPlayerSkia.cpp
    DisplayListRasterCache.cpp
    FontSkia.cpp
    ConnectionFromClient.cpp
    ConnectionFromPresentation.cpp
    ConnectionFromWebContent.cpp
    ElasticOverscrollController.cpp
    FramePacer.cpp
    HostWebGLContext.cpp
    KeyboardScrollController.cpp
    OpenGLContext.cpp
    PainterSkia.cpp
    PaintingSurface.cpp
    PausedDebuggerOverlay.cpp
    PictureInPictureControls.cpp
    RasterizeDisplayList.cpp
    RenderClockConnection.cpp
    ScrollFlingController.cpp
    ScrollSnapController.cpp
    ScrollbarController.cpp
    SkiaBackendContext.cpp
    SkiaUtils.cpp
    VideoFrameHolds.cpp
    VideoFrameSkia.cpp
    VSyncScheduler.cpp
    WebGLObjectMap.cpp
    WheelGestureRouting.cpp
)

invoke_py_generator(
    "WebGLCommandReplayer.cpp"
    "generate_compositor_webgl_replayer.py"
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibCompositing/WebGL/GLFunctions.json"
    "WebGLCommandReplayer.h"
    "WebGLCommandReplayer.cpp"
    arguments -j "${LADYBIRD_SOURCE_DIR}/Libraries/LibCompositing/WebGL/GLFunctions.json"
    dependencies "${LADYBIRD_SOURCE_DIR}/Meta/Generators/libweb_webgl.py"
)

set(GENERATED_SOURCES
    CompositorControlClientEndpoint.h
    CompositorControlServerEndpoint.h
    CompositorPresentationClientEndpoint.h
    CompositorPresentationServerEndpoint.h
    CompositorWebContentClientEndpoint.h
    CompositorWebContentServerEndpoint.h
    RenderClockClientEndpoint.h
    RenderClockServerEndpoint.h
    ${CMAKE_BINARY_DIR}/Libraries/LibCompositing/WebGL/GLFunctions.cpp
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

target_link_libraries(Compositor PRIVATE compositorservice LibCore LibMain LibSandbox)
# ANGLE must precede Skia so macOS GLES symbols bind to ANGLE rather than the
# OpenGL framework pulled in by Skia.
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibIPC LibMedia LibCompositing ${ANGLE_TARGETS} skia)

if (HAS_VULKAN)
    target_sources(compositorservice PRIVATE SkiaVulkanMemoryAllocator.cpp)
    target_link_libraries(compositorservice PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    target_sources(compositorservice PRIVATE VideoFrameSkiaVulkan.cpp)
endif()

if (APPLE)
    target_sources(compositorservice PRIVATE VideoFrameSkia.mm)
    target_link_libraries(compositorservice PUBLIC "-framework CoreGraphics" "-framework CoreVideo")
endif()

if (WIN32)
    ladybird_windows_bin(Compositor CONSOLE)
endif()

ladybird_sign_helper_process(Compositor)
