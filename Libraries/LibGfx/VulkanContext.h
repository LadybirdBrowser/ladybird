/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN

#    include <AK/Assertions.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>
#    include <vulkan/vulkan.h>
#    if defined(USE_VULKAN_IMAGES)
#        include <libdrm/drm_fourcc.h>
#    endif

namespace Gfx {

struct VulkanContext {
    uint32_t api_version { VK_API_VERSION_1_0 };
    VkInstance instance { VK_NULL_HANDLE };
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    VkDevice logical_device { VK_NULL_HANDLE };
    VkQueue graphics_queue { VK_NULL_HANDLE };
    uint32_t graphics_queue_family { 0 };
#    ifdef USE_VULKAN_IMAGES
    VkCommandPool command_pool { VK_NULL_HANDLE };
    VkCommandBuffer command_buffer { VK_NULL_HANDLE };
    struct
    {
        PFN_vkGetMemoryFdKHR get_memory_fd { nullptr };
        PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties { nullptr };
    } ext_procs;
#    endif
};

ErrorOr<VulkanContext> create_vulkan_context();

#    ifdef USE_VULKAN_IMAGES
struct VulkanImage : public RefCounted<VulkanImage> {
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    struct {
        VkFormat format;
        VkExtent3D extent;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        VkSharingMode sharing_mode;
        VkImageLayout layout;
        VkDeviceSize row_pitch; // for tiled images this is some implementation-specific value
        uint64_t modifier { DRM_FORMAT_MOD_INVALID };
    } info;
    VulkanContext const& context;

    int get_dma_buf_fd();
    void transition_layout(VkImageLayout old_layout, VkImageLayout new_layout);
    VulkanImage(VulkanContext const& context)
        : context(context)
    {
    }
    ~VulkanImage();
};

static inline uint32_t vk_format_to_drm_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DRM_FORMAT_ARGB8888;
    // add more as needed
    default:
        VERIFY_NOT_REACHED();
        return DRM_FORMAT_INVALID;
    }
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, uint32_t num_modifiers, uint64_t const* modifiers);
#    endif

}

#endif
