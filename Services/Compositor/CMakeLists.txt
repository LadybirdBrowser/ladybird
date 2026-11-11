set(SOURCES
    AnimatedImage.cpp
    BackingStoreManager.cpp
    BitmapExport.cpp
    Canvas2DCommandPlayer.cpp
    CanvasHost.cpp
    ColorSpaceSkia.cpp
    CompositorCore.cpp
    CompositorState.cpp
    ContextPresenter.cpp
    DisplayListPlayer.cpp
    DisplayListPlayerSkia.cpp
    DisplayListRasterCache.cpp
    FontSkia.cpp
    FrameInputs.cpp
    ConnectionFromClient.cpp
    ConnectionFromPresentation.cpp
    ConnectionFromWebContent.cpp
    FramePacer.cpp
    HostWebGLContext.cpp
    OpenGLContext.cpp
    PaintingSurface.cpp
    PausedDebuggerOverlay.cpp
    PictureInPictureControls.cpp
    RasterizeDisplayList.cpp
    RenderClockConnection.cpp
    SkiaBackendContext.cpp
    SkiaUtils.cpp
    VideoFrameHolds.cpp
    VideoFrameSkia.cpp
    VSyncScheduler.cpp
    WebGLObjectMap.cpp
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

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME compositor_core FEATURES allocator FFI_HEADER CompositorCoreRustFFI.h)
target_link_libraries(compositor_core INTERFACE LibGfx)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME compositor_core)

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
target_link_libraries(compositorservice PRIVATE LibCore LibGfx LibImageDecoderClient LibIPC LibMedia LibCompositing compositor_core ${ANGLE_TARGETS} skia)

if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(compositorservice INTERFACE LINKER:--allow-multiple-definition)
endif()

if (HAS_FONTCONFIG)
    target_link_libraries(compositorservice PRIVATE Fontconfig::Fontconfig)
    target_link_libraries(Compositor PRIVATE Fontconfig::Fontconfig)
endif()

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
