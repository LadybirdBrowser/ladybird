/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Error.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>
#    include <AK/Vector.h>
#    include <LibGfx/Forward.h>
#    include <LibGfx/LinuxDmaBuf.h>
#    include <libdrm/drm_fourcc.h>
#    include <vulkan/vulkan.h>

namespace Gfx {

class VulkanImage : public RefCounted<VulkanImage> {
public:
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

    explicit VulkanImage(VulkanContext const& context);
    ~VulkanImage();

    int get_dma_buf_fd();
    void transition_layout(VkImageLayout old_layout, VkImageLayout new_layout);
};

ErrorOr<VkFormat> drm_format_to_vk_format(u32 drm_format);
ErrorOr<BitmapFormat> vk_format_to_bitmap_format(VkFormat format);

uint32_t vk_format_to_drm_format(VkFormat format);

ErrorOr<NonnullRefPtr<VulkanImage>> create_dma_buf_vulkan_image(
    VulkanContext const& context,
    IntSize size,
    VkFormat format,
    bool cpu_mappable);

ErrorOr<NonnullRefPtr<VulkanImage>> create_dma_buf_vulkan_image(
    VulkanContext const& context,
    IntSize size,
    VkFormat format,
    VkMemoryPropertyFlags required_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    VkMemoryPropertyFlags preferred_flags = 0,
    ReadonlySpan<uint64_t> allowed_modifiers = {});

ErrorOr<NonnullRefPtr<VulkanImage>> import_vulkan_image_from_dma_buf(
    VulkanContext const& context,
    IntSize size,
    VkFormat format,
    u64 modifier,
    LinuxDmaBufPlaneLayout const& plane,
    int fd);

}
#endif
