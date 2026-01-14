include_guard()

if (NOT APPLE)
    find_package(VulkanHeaders CONFIG QUIET)
    find_package(Vulkan QUIET)
    if (VulkanHeaders_FOUND AND Vulkan_FOUND)
        set(HAS_VULKAN ON CACHE BOOL "" FORCE)
        add_cxx_compile_definitions(USE_VULKAN=1)

        # Sharable Vulkan images are currently only implemented on Linux and FreeBSD
        if ((LINUX AND NOT ANDROID) OR CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
            set(USE_VULKAN_IMAGES ON CACHE BOOL "" FORCE)
            add_cxx_compile_definitions(USE_VULKAN_IMAGES=1)
        endif()
    endif()
endif()
