if (NOT ENABLE_WEBGPUNATIVE)
    message(WARNING "No backend implementation for WebGPUNative available")
    return()
endif()

set(SOURCES)
serenity_lib(LibWebGPUNative webgpunative EXPLICIT_SYMBOL_EXPORT)
target_compile_definitions(LibWebGPUNative PRIVATE $<$<CONFIG:Debug>:WEBGPUNATIVE_DEBUG>)
target_link_libraries(LibWebGPUNative PRIVATE AK LibCore)

if (ENABLE_WEBGPUNATIVE_METAL_IMPL)
    set(WEBGPUNATIVE_METAL_BRIDGE_SOURCES
        Metal/AdapterImpl.mm
        Metal/DeviceImpl.mm
    )
    target_sources(LibWebGPUNative
        PRIVATE
            Metal/Adapter.cpp
            Metal/AdapterImpl.swift
            Metal/CommandBuffer.cpp
            Metal/CommandBufferImpl.mm
            Metal/CommandEncoder.cpp
            Metal/CommandEncoderImpl.mm
            Metal/Device.cpp
            Metal/DeviceImpl.swift
            Metal/Handle.mm
            Metal/Instance.cpp
            Metal/Queue.cpp
            Metal/QueueImpl.mm
            Metal/Texture.cpp
            Metal/TextureImpl.mm
            Metal/TextureView.cpp
            Metal/TextureViewImpl.mm
            ${WEBGPUNATIVE_METAL_BRIDGE_SOURCES}
    )
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    # NOTE: Required for (__bridge_transfer NSString *) call in generated swift bindings header, otherwise build fails due to -Warc-bridge-casts-disallowed-in-nonarc
    set_source_files_properties(${WEBGPUNATIVE_METAL_BRIDGE_SOURCES}
        PROPERTIES
            COMPILE_FLAGS
                "-Wno-arc-bridge-casts-disallowed-in-nonarc"
    )
    add_swift_target_properties(LibWebGPUNative LAGOM_LIBRARIES AK LibCore)
    generate_clang_module_map(LibWebGPUNative)
elseif (ENABLE_WEBGPUNATIVE_DIRECTX_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            DirectX/Adapter.cpp
            DirectX/AdapterImpl.cpp
            DirectX/CommandBuffer.cpp
            DirectX/CommandBufferImpl.cpp
            DirectX/CommandEncoder.cpp
            DirectX/CommandEncoderImpl.cpp
            DirectX/Device.cpp
            DirectX/DeviceImpl.cpp
            DirectX/Instance.cpp
            DirectX/Queue.cpp
            DirectX/QueueImpl.cpp
            DirectX/Texture.cpp
            DirectX/TextureImpl.cpp
            DirectX/TextureView.cpp
            DirectX/TextureViewImpl.cpp
    )
    find_package(directx-headers CONFIG REQUIRED)
    find_package(directx12-agility CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE d3d12 dxgi Microsoft::DirectX-Headers Microsoft::DirectX-Guids Microsoft::DirectX12-Agility)
    file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12")
    add_custom_command(TARGET LibWebGPUNative POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE> $<TARGET_FILE_DIR:LibWebGPUNative>/D3D12
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG> $<TARGET_FILE_DIR:LibWebGPUNative>/D3D12
            COMMAND_EXPAND_LISTS
    )
elseif (ENABLE_WEBGPUNATIVE_VULKAN_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            Vulkan/Adapter.cpp
            Vulkan/AdapterImpl.cpp
            Vulkan/CommandBuffer.cpp
            Vulkan/CommandBufferImpl.cpp
            Vulkan/CommandEncoder.cpp
            Vulkan/CommandEncoderImpl.cpp
            Vulkan/Device.cpp
            Vulkan/DeviceImpl.cpp
            Vulkan/Instance.cpp
            Vulkan/InstanceImpl.cpp
            Vulkan/Queue.cpp
            Vulkan/QueueImpl.cpp
            Vulkan/Texture.cpp
            Vulkan/TextureImpl.cpp
            Vulkan/TextureView.cpp
            Vulkan/TextureViewImpl.cpp
    )
    find_package(Vulkan REQUIRED)
    find_package(VulkanUtilityLibraries CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE Vulkan::Vulkan Vulkan::UtilityHeaders vk-bootstrap::vk-bootstrap)
endif()
