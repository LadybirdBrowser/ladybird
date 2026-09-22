include(libcompositing_generators)

set(SOURCES
    DisplayList/AccumulatedVisualContext.cpp
    DisplayList/Canvas2DCommandStream.cpp
    DisplayList/DisplayList.cpp
    DisplayList/DisplayListDamage.cpp
    DisplayList/DisplayListPlayerSkia.cpp
    DisplayList/DisplayListResourceStorage.cpp
    DisplayList/DisplayListResourceTransaction.cpp
    DisplayList/RustFFIAssertions.cpp
    DisplayList/VisualContextTreeTestBuilder.cpp
    InputEvent.cpp
    PixelUnits.cpp
    Scrolling/AsyncScrollTree.cpp
    Scrolling/AsyncScrollingState.cpp
    Scrolling/ScrollSnapSelection.cpp
    Scrolling/ScrollState.cpp
    Scrolling/SmoothScrollAnimation.cpp
    Scrolling/WheelGestureIdentity.cpp
    Types.cpp
    WebGL/TextureUpload.cpp
    WebGL/WebGLCommandList.cpp
)

generate_compositing_webgl_implementation()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/Compositor)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorFontClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorFontClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorFontServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorFontServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentServerEndpoint.h)

set(GENERATED_SOURCES
    WebGL/WebGLCommands.cpp
    ../../Services/Compositor/CompositorControlClientEndpoint.h
    ../../Services/Compositor/CompositorControlServerEndpoint.h
    ../../Services/Compositor/CompositorFontClientEndpoint.h
    ../../Services/Compositor/CompositorFontServerEndpoint.h
    ../../Services/Compositor/CompositorWebContentClientEndpoint.h
    ../../Services/Compositor/CompositorWebContentServerEndpoint.h
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
target_link_libraries(LibCompositing PRIVATE LibCore LibGfx LibIPC LibMedia skia libcompositing_rust)

# The WebGL command stream headers include ANGLE's GLES headers for the GL constants and types,
# but the GL entry points live in the Compositor. Hand out only ANGLE's compile interface, to this
# library and to whatever includes its headers, so nothing that links LibCompositing pulls ANGLE in.
foreach(angle_target IN LISTS ANGLE_TARGETS)
    target_include_directories(LibCompositing PUBLIC $<TARGET_PROPERTY:${angle_target},INTERFACE_INCLUDE_DIRECTORIES>)
    target_include_directories(LibCompositing SYSTEM PUBLIC $<TARGET_PROPERTY:${angle_target},INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
    target_compile_definitions(LibCompositing PUBLIC $<TARGET_PROPERTY:${angle_target},INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_options(LibCompositing PRIVATE $<TARGET_PROPERTY:${angle_target},INTERFACE_COMPILE_OPTIONS>)
endforeach()

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
    foreach(bridge IN ITEMS DisplayList/AccumulatedVisualContext.cpp DisplayList/DisplayList.cpp DisplayList/DisplayListDamage.cpp DisplayList/DisplayListResourceStorage.cpp DisplayList/VisualContextTreeTestBuilder.cpp PixelUnits.cpp)
        set_property(SOURCE ${bridge} APPEND PROPERTY OBJECT_DEPENDS ${_libcompositing_rust_lib})
    endforeach()
endif()
