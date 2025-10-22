include_guard()

if (NOT APPLE)
    find_package(VulkanHeaders CONFIG QUIET)
    find_package(Vulkan QUIET)
    if (VulkanHeaders_FOUND AND Vulkan_FOUND)
        set(HAS_VULKAN ON CACHE BOOL "" FORCE)
        add_cxx_compile_definitions(USE_VULKAN=1)

        set(TRY_RUN_BINARY_DIR "${CMAKE_SOURCE_DIR}/Meta/CMake/TryRunChecks")
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer-no-link")
        try_run(
            DRM_CHECK_RUN_RESULT
            DRM_CHECK_COMPILE_RESULT
            "${TRY_RUN_BINARY_DIR}"
            "${CMAKE_SOURCE_DIR}/Meta/CMake/vulkan-drm-checker.cpp"
            LINK_LIBRARIES Vulkan::Vulkan
            CMAKE_FLAGS
                "-DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}"
            OUTPUT_VARIABLE DRM_CHECK_TRY_RUN_OUTPUT
        )
        unset(CMAKE_REQUIRED_FLAGS)

        message(STATUS "${DRM_CHECK_TRY_RUN_OUTPUT}")

        if (NOT DRM_CHECK_COMPILE_RESULT)
           message(FATAL_ERROR "Could not build Vulkan DRM checker")
        endif()

        if (DRM_CHECK_RUN_RESULT EQUAL 0)
            message(STATUS "Shareable Vulkan Images available")
            add_cxx_compile_definitions(USE_VULKAN_IMAGES=1)
        else()
            message(STATUS "Shareable Vulkan Images NOT available")
        endif()
    endif()
endif()
