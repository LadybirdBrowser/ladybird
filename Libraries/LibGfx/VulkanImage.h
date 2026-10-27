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
#    include <AK/Span.h>
#    include <AK/Vector.h>
#    include <LibGfx/Forward.h>
#    include <LibGfx/LinuxDmaBufHandle.h>
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

    int get_dma_buf_fd() const;
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

// The DRM format modifiers of the single-plane layouts that the device can both render to and sample from.
Vector<uint64_t> renderable_drm_format_modifiers(VulkanContext const& context, VkFormat format);
// Whether the device can import a dma-buf in this format and layout as an image for these uses.
bool can_import_dmabuf_image(VkPhysicalDevice, VkFormat, uint64_t modifier, VkImageUsageFlags);

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, ReadonlySpan<uint64_t> modifiers);

struct ImportedDmaBufImage {
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
};
// Imports a single-plane dma-buf as an image in the exact layout it has, for these uses. The caller owns the image and
// its memory.
ErrorOr<ImportedDmaBufImage> import_linux_dmabuf_image(VkPhysicalDevice, VkDevice, PFN_vkGetMemoryFdPropertiesKHR, LinuxDmaBufHandle const&, VkFormat, VkImageUsageFlags);

// Copies the pixels of a single-plane BGRA dma-buf that the device can import, in whatever layout it has, into a bitmap.
ErrorOr<NonnullRefPtr<Bitmap>> read_linux_dmabuf_into_bitmap(VulkanContext const& context, LinuxDmaBufHandle const& dmabuf);

}

#endif
