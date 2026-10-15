/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Noncopyable.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/Vector.h>
#    include <LibGfx/LinuxDmaBufHandle.h>
#    include <LibGfx/VulkanContext.h>

namespace Gfx {

// A YCbCr image that another process decoded into dma-bufs, imported to be sampled through a YCbCr conversion. The
// imported memory keeps the dma-bufs alive for as long as the image exists.
class ImportedYCbCrVulkanImage {
    AK_MAKE_NONCOPYABLE(ImportedYCbCrVulkanImage);
    AK_MAKE_NONMOVABLE(ImportedYCbCrVulkanImage);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct ImportedMemory {
        VkDeviceMemory memory { VK_NULL_HANDLE };
        VkDeviceSize size { 0 };
    };

    // Skia wraps an image only if it can also copy to and from it.
    static constexpr VkImageUsageFlags USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // What the format must support under a modifier for an image to be imported with it, which a process exporting
    // images to import here can choose its modifiers by.
    static constexpr VkFormatFeatureFlags REQUIRED_FORMAT_FEATURES = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    static ErrorOr<NonnullOwnPtr<ImportedYCbCrVulkanImage>> import_dma_buf(VulkanContext const&, LinuxDmaBufHandle const&);

    ~ImportedYCbCrVulkanImage();

    VkImage image() const { return m_image; }
    VkFormat format() const { return m_format; }
    // What the format can do under the dma-buf's modifier, which limits the chroma siting and filtering that a
    // conversion sampling it may use.
    VkFormatFeatureFlags format_features() const { return m_format_features; }
    // One memory per plane when the planes lie in separate dma-bufs, and one for the whole image otherwise.
    ReadonlySpan<ImportedMemory> memories() const { return m_memories; }
    bool is_disjoint() const { return m_is_disjoint; }

private:
    explicit ImportedYCbCrVulkanImage(VkDevice device)
        : m_device(device)
    {
    }

    VkDevice m_device { VK_NULL_HANDLE };
    VkImage m_image { VK_NULL_HANDLE };
    VkFormat m_format { VK_FORMAT_UNDEFINED };
    VkFormatFeatureFlags m_format_features { 0 };
    Vector<ImportedMemory, LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT> m_memories;
    bool m_is_disjoint { false };
};

}

#endif
