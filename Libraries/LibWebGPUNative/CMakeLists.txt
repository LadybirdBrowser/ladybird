if (NOT ENABLE_WEBGPUNATIVE)
    message(WARNING "No backend implementation for WebGPUNative available")
    return()
endif()

set(SOURCES)
ladybird_lib(LibWebGPUNative webgpunative EXPLICIT_SYMBOL_EXPORT)
target_compile_definitions(LibWebGPUNative PRIVATE $<$<CONFIG:Debug>:WEBGPUNATIVE_DEBUG>)
target_link_libraries(LibWebGPUNative PRIVATE AK LibCore)

if (ENABLE_WEBGPUNATIVE_VULKAN_IMPL)
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
            Vulkan/RenderPassEncoder.cpp
            Vulkan/RenderPassEncoderImpl.cpp
            Vulkan/Texture.cpp
            Vulkan/TextureImpl.cpp
            Vulkan/TextureView.cpp
            Vulkan/TextureViewImpl.cpp
    )
    find_package(Vulkan REQUIRED)
    find_package(VulkanUtilityLibraries CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    target_link_libraries(LibWebGPUNative PRIVATE Vulkan::Vulkan Vulkan::UtilityHeaders vk-bootstrap::vk-bootstrap)
    target_compile_definitions(LibWebGPUNative PUBLIC WEBGPUNATIVE_VULKAN)
    if (WIN32)
        # FIXME: Build vcpkg libraries with clang-cl so they can also build in sanitize. Currently the VkBootstrap lib
        #  causes the following linker error in Windows_Sanitizer* presets only:
        #   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
        #   >>> Lagom\Libraries\LibWebGPUNative\CMakeFiles\LibWebGPUNative.dir\Vulkan\TextureViewImpl.cpp.obj has value 1
        #   >>> vk-bootstrap.lib(VkBootstrap.cpp.obj) has value 0
        # https://learn.microsoft.com/en-us/answers/questions/864574/enabling-address-sanitizer-results-in-error-lnk203
        if (ENABLE_WINDOWS_CI)
            # We have to disable ASAN for the full target as fixing the issue in the initial TextureViewImpl.cpp TU
            #  alone causes transitive issues in other TUs that depend on it
            target_compile_options(LibWebGPUNative PRIVATE -fno-sanitize=address)
        endif()
    endif()
endif()
