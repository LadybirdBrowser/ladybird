if (NOT ENABLE_WEBGPUNATIVE)
    message(WARNING "No backend implementation for WebGPUNative available")
    return()
endif()

set(SOURCES)
ladybird_lib(LibWebGPUNative webgpunative EXPLICIT_SYMBOL_EXPORT)
target_compile_definitions(LibWebGPUNative PRIVATE $<$<CONFIG:Debug>:WEBGPUNATIVE_DEBUG>)
target_link_libraries(LibWebGPUNative PRIVATE AK)

if (ENABLE_WEBGPUNATIVE_METAL_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            Metal/Instance.cpp
    )
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    target_compile_definitions(LibWebGPUNative PUBLIC WEBGPUNATIVE_METAL)
elseif (ENABLE_WEBGPUNATIVE_VULKAN_IMPL)
    target_sources(LibWebGPUNative
        PRIVATE
            Vulkan/Instance.cpp
            Vulkan/InstanceImpl.cpp
    )
    find_package(Vulkan REQUIRED)
    find_package(VulkanUtilityLibraries CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE Vulkan::Vulkan Vulkan::UtilityHeaders vk-bootstrap::vk-bootstrap)
    target_compile_definitions(LibWebGPUNative PUBLIC WEBGPUNATIVE_VULKAN)
endif()
