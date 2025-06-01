if (NOT ENABLE_WEBGPUNATIVE)
    message(WARNING "No backend implementation for WebGPUNative available")
    return()
endif()

set(SOURCES)
serenity_lib(LibWebGPUNative webgpunative EXPLICIT_SYMBOL_EXPORT)
target_compile_definitions(LibWebGPUNative PRIVATE $<$<CONFIG:Debug>:WEBGPUNATIVE_DEBUG>)
target_link_libraries(LibWebGPUNative PRIVATE AK LibCore)

if (ENABLE_WEBGPUNATIVE_METAL_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            Metal/Adapter.cpp
            Metal/AdapterImpl.mm
            Metal/AdapterImpl.swift
            Metal/CommandBuffer.cpp
            Metal/CommandBufferImpl.mm
            Metal/Device.cpp
            Metal/DeviceImpl.mm
            Metal/Handle.mm
            Metal/Instance.cpp
            Metal/Queue.cpp
            Metal/QueueImpl.mm
    )
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    # FIXME: Generated header file uses __bridge_transfer which results in an error due to -Warc-bridge-casts-disallowed-in-nonarc, figure out how to remove this from the generation, as right now it needs to be manually removed
    add_swift_target_properties(LibWebGPUNative LAGOM_LIBRARIES AK LibCore)
    generate_clang_module_map(LibWebGPUNative)
elseif (ENABLE_WEBGPUNATIVE_DIRECTX_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            DirectX/Adapter.cpp
            DirectX/AdapterImpl.cpp
            DirectX/Device.cpp
            DirectX/DeviceImpl.cpp
            DirectX/Instance.cpp
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
            Vulkan/Device.cpp
            Vulkan/DeviceImpl.cpp
            Vulkan/Instance.cpp
            Vulkan/InstanceImpl.cpp
            Vulkan/Queue.cpp
            Vulkan/QueueImpl.cpp
    )
    find_package(Vulkan REQUIRED)
    find_package(VulkanUtilityLibraries CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE Vulkan::Vulkan Vulkan::UtilityHeaders vk-bootstrap::vk-bootstrap)
endif()
