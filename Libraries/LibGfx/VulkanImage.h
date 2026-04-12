/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Assertions.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>
#    include <LibGfx/VulkanContext.h>
#    include <libdrm/drm_fourcc.h>

namespace Gfx {

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

}

#endif
