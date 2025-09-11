include_guard()

if (NOT APPLE)
    find_package(VulkanHeaders CONFIG QUIET)
    find_package(Vulkan QUIET)
    if (VulkanHeaders_FOUND AND Vulkan_FOUND)
        set(HAS_VULKAN ON CACHE BOOL "" FORCE)
        add_cxx_compile_definitions(USE_VULKAN=1)

        set(TRY_RUN_BINARY_DIR "${CMAKE_SOURCE_DIR}/Meta/CMake/TryRunChecks")
        try_run(
            EXTENSION_CHECK_RUN_RESULT
            EXTENSION_CHECK_COMPILE_RESULT
            "${TRY_RUN_BINARY_DIR}"
            SOURCES "${CMAKE_SOURCE_DIR}/Meta/CMake/ladybird-vulkan-checker.cpp"
            LINK_LIBRARIES Vulkan::Vulkan
            CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}"
        )

        if (EXTENSION_CHECK_RUN_RESULT EQUAL 0)
            add_cxx_compile_definitions(USE_VULKAN_IMAGES=1)
        endif()
    endif()
endif()
