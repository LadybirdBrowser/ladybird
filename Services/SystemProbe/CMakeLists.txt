set(SOURCES
    main.cpp
)

if (HAS_VULKAN)
    list(APPEND SOURCES VulkanDescription.cpp)
endif()

add_executable(SystemProbe ${SOURCES})

target_include_directories(SystemProbe PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(SystemProbe PRIVATE LibCore LibGfx LibMain LibPlatformFeatures)
if (HAS_VULKAN)
    target_link_libraries(SystemProbe PRIVATE Vulkan::Vulkan)
endif()

ladybird_sign_helper_process(SystemProbe)
