if (NOT ENABLE_WEBGPUNATIVE)
    message(WARNING "No backend implementation for WebGPUNative available")
    return()
endif()

set(SOURCES)
ladybird_lib(LibWebGPUNative webgpunative EXPLICIT_SYMBOL_EXPORT)
target_compile_definitions(LibWebGPUNative PRIVATE $<$<CONFIG:Debug>:WEBGPUNATIVE_DEBUG>)
target_link_libraries(LibWebGPUNative PRIVATE AK LibCore)

if (ENABLE_WEBGPUNATIVE_METAL_IMPL)
    set(WEBGPUNATIVE_METAL_BRIDGE_SOURCES
        Metal/AdapterImpl.mm
    )
    target_sources(LibWebGPUNative
        PRIVATE
            Metal/Adapter.cpp
            Metal/AdapterImpl.swift
            Metal/Instance.cpp
            ${WEBGPUNATIVE_METAL_BRIDGE_SOURCES}
    )
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    target_compile_definitions(LibWebGPUNative PUBLIC WEBGPUNATIVE_METAL)
    # NOTE: Required for (__bridge_transfer NSString *) call in generated swift bindings header, otherwise build fails due to -Warc-bridge-casts-disallowed-in-nonarc
    set_source_files_properties(${WEBGPUNATIVE_METAL_BRIDGE_SOURCES}
        PROPERTIES
            COMPILE_FLAGS
                "-Wno-arc-bridge-casts-disallowed-in-nonarc"
    )
    add_swift_target_properties(LibWebGPUNative LAGOM_LIBRARIES AK LibCore)
    generate_clang_module_map(LibWebGPUNative)
elseif (ENABLE_WEBGPUNATIVE_VULKAN_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            Vulkan/Adapter.cpp
            Vulkan/AdapterImpl.cpp
            Vulkan/Device.cpp
            Vulkan/DeviceImpl.cpp
            Vulkan/Instance.cpp
            Vulkan/InstanceImpl.cpp
    )
    find_package(Vulkan REQUIRED)
    find_package(VulkanUtilityLibraries CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE Vulkan::Vulkan Vulkan::UtilityHeaders vk-bootstrap::vk-bootstrap)
    target_compile_definitions(LibWebGPUNative PUBLIC WEBGPUNATIVE_VULKAN)
endif()
