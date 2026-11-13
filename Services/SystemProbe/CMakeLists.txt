set(SOURCES
    main.cpp
)

if (HAS_VULKAN)
    list(APPEND SOURCES VulkanDescription.cpp)
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    list(APPEND SOURCES FFmpegDescription.cpp VulkanVideoDescription.cpp)
endif()

add_executable(SystemProbe ${SOURCES})

target_include_directories(SystemProbe PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(SystemProbe PRIVATE LibCore LibGfx LibMain LibPlatformFeatures)
if (HAS_VULKAN)
    target_link_libraries(SystemProbe PRIVATE Vulkan::Vulkan)
endif()
if (USE_VULKAN_DMABUF_IMAGES)
    target_link_libraries(SystemProbe PRIVATE LibMediaCodecs)
endif()

ladybird_sign_helper_process(SystemProbe)
