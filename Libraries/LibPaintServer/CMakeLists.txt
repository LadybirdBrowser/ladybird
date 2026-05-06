set(SOURCES
    ArenaWriter.cpp
    ArenaSubmitter.cpp
    BrokerOfPaintServer.cpp
    Compositor/CanvasPainter.cpp
    Compositor/DrawList.cpp
    Compositor/DrawCommands.cpp
    Debug.cpp
    RenderClientOfPaintServer.cpp
    Resource/SharedBlob.cpp
    Resource/SharedArena.cpp
)

if (APPLE)
    list(APPEND SOURCES
        Platform/MetalPresentation.cpp
    )
else()
    list(APPEND SOURCES
        Platform/VulkanPresentation.cpp
    )
endif()

set(GENERATED_SOURCES
    ToBrokerFromPaintServerEndpoint.h
    ToPaintServerFromBrokerEndpoint.h
    ToRenderClientFromPaintServerEndpoint.h
    ToPaintServerFromRenderClientEndpoint.h
)

compile_ipc(./ToPaintServerFromBroker.ipc ${CMAKE_BINARY_DIR}/Libraries/LibPaintServer/ToPaintServerFromBrokerEndpoint.h)
compile_ipc(./ToBrokerFromPaintServer.ipc ${CMAKE_BINARY_DIR}/Libraries/LibPaintServer/ToBrokerFromPaintServerEndpoint.h)
compile_ipc(./ToPaintServerFromRenderClient.ipc ${CMAKE_BINARY_DIR}/Libraries/LibPaintServer/ToPaintServerFromRenderClientEndpoint.h)
compile_ipc(./ToRenderClientFromPaintServer.ipc ${CMAKE_BINARY_DIR}/Libraries/LibPaintServer/ToRenderClientFromPaintServerEndpoint.h)

ladybird_lib(LibPaintServer PaintServer)
target_link_libraries(LibPaintServer PRIVATE LibCore LibGfx LibIPC LibMain skia)

if (APPLE)
    target_link_libraries(LibPaintServer PRIVATE LibThreading)
endif()
