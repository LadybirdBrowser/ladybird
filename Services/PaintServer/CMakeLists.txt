set(SOURCES
    BrokerConnection.cpp
    Compositor/DisplayListPlayer.cpp
    Compositor/DrawCommandPlayer.cpp
    FontResourceCache.cpp
    Painter.cpp
    main.cpp
    PresentationBufferState.cpp
    RenderClient/CompletionTracker.cpp
    RenderClient/RenderClientConnection.cpp
    RenderClient/ResourceManager.cpp
    Server.cpp
    WorkerThread.cpp
)

if (APPLE)
    list(APPEND SOURCES Platform/MetalPainter.cpp)
elseif (USE_VULKAN_DMABUF_IMAGES)
    list(APPEND SOURCES Platform/VulkanPainter.cpp)
    #set(PaintServer_PLATFORM_DEFINES USE_VULKAN=1 USE_VULKAN_DMABUF_IMAGES=1)
else()
    list(APPEND SOURCES Platform/StubDevicePainter.cpp)
endif()

add_executable(PaintServer ${SOURCES})
target_link_libraries(PaintServer PRIVATE LibCore LibIPC LibMain LibPaintServer LibGfx skia)

if (APPLE)
    find_package(Fontconfig REQUIRED)
    target_link_libraries(PaintServer PRIVATE Fontconfig::Fontconfig)
elseif (HAS_FONTCONFIG)
    target_link_libraries(PaintServer PRIVATE Fontconfig::Fontconfig)
endif()

target_link_libraries(PaintServer PRIVATE LibThreading)

if (WIN32)
    lagom_windows_bin(PaintServer CONSOLE)
endif()
