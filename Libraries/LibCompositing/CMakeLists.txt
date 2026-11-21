set(SOURCES
    CanvasColorSpace.cpp
    DisplayList/DisplayListResourceEncoding.cpp
    DisplayList/DisplayListResources.cpp
    DisplayList/RegisteredImageFrames.cpp
    DisplayList/RegisteredTypefaces.cpp
    DisplayList/RustFFIAssertions.cpp
    FontServiceClient.cpp
    Scrolling/SmoothScrollAnimation.cpp
    UIOverlays.cpp
    Video/VideoFrameHold.cpp
    Video/VideoFramePresentationPage.cpp
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/Compositor)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorPresentationClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorPresentationClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorPresentationServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorPresentationServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/RenderClockClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/RenderClockClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/RenderClockServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/RenderClockServerEndpoint.h)
compile_ipc(Types.ipc TypesCodecs.h)
compile_ipc(FontClient.ipc FontClientEndpoint.h)
compile_ipc(FontServer.ipc FontServerEndpoint.h)

set(GENERATED_SOURCES
    ../../Services/Compositor/CompositorControlClientEndpoint.h
    ../../Services/Compositor/CompositorControlServerEndpoint.h
    ../../Services/Compositor/CompositorPresentationClientEndpoint.h
    ../../Services/Compositor/CompositorPresentationServerEndpoint.h
    ../../Services/Compositor/CompositorWebContentClientEndpoint.h
    ../../Services/Compositor/CompositorWebContentServerEndpoint.h
    ../../Services/Compositor/RenderClockClientEndpoint.h
    ../../Services/Compositor/RenderClockServerEndpoint.h
    FontClientEndpoint.h
    FontServerEndpoint.h
)

import_rust_crate(
    MANIFEST_PATH Rust/Cargo.toml
    CRATE_NAME libcompositing_rust
    FEATURES allocator
    FFI_HEADERS
        RustFFI.h
        TypesRustFFI.h
        DisplayList/DisplayListCommandsGenerated.h
)
target_link_libraries(libcompositing_rust INTERFACE LibGfx)
test_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libcompositing_rust)

ladybird_lib(LibCompositing compositing EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibCompositing PUBLIC LibWebCommon)
target_link_libraries(LibCompositing PRIVATE LibCore LibGfx LibIPC LibMedia LibThreading libcompositing_rust)

get_target_property(_libcompositing_rust_lib libcompositing_rust IMPORTED_LOCATION)
if ((LINUX OR BSD) AND NOT BUILD_SHARED_LIBS)
    target_link_options(LibCompositing INTERFACE LINKER:--allow-multiple-definition)
endif()
if ((LINUX OR BSD) AND BUILD_SHARED_LIBS)
    # The crate is an implementation detail of this library, and LibWeb carries its own copy of the
    # same entry points. Keep the crate's symbols, including its copy of the Rust standard library,
    # out of the dynamic symbol table: ELF symbol interposition would otherwise let the other Rust
    # standard library copies in the process bind their panic-hook state to this one.
    target_link_options(LibCompositing PRIVATE LINKER:--exclude-libs=liblibcompositing_rust.a)
endif()

if(NOT BUILD_SHARED_LIBS)
    add_custom_command(TARGET LibCompositing POST_BUILD
        COMMAND ${CMAKE_AR} -x $<TARGET_FILE:libcompositing_rust>
        COMMAND ${CMAKE_AR} -qS $<TARGET_FILE:LibCompositing> *.o
        COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:LibCompositing>
        COMMAND ${CMAKE_COMMAND} -E remove -f *.o
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rust_merge_tmp
        COMMENT "Merging Rust archive into LibCompositing"
    )
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rust_merge_tmp)

    # POST_BUILD steps don't track inputs, so the bridge files depend on the Rust archive: when it
    # changes, LibCompositing re-archives and the merge picks up the fresh objects.
    foreach(bridge IN ITEMS DisplayList/RegisteredImageFrames.cpp Scrolling/SmoothScrollAnimation.cpp)
        set_property(SOURCE ${bridge} APPEND PROPERTY OBJECT_DEPENDS ${_libcompositing_rust_lib})
    endforeach()
endif()
