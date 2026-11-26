/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/AtomicRefCounted.h>
#    include <AK/Error.h>
#    include <AK/Mutex.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/Vector.h>
#    include <LibGfx/Size.h>
#    include <LibMediaCodecs/Export.h>
#    include <LibMediaCodecs/VideoSurface.h>
#    include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#    include <vulkan/vulkan.h>

namespace Media {

// Images of one format and size that export as dma-bufs, for decoded frames to be copied into. An image is exported
// once, and recycled when the frame it went out in is released.
class MEDIACODECS_API VulkanExportableImagePool final : public AtomicRefCounted<VulkanExportableImagePool> {
public:
    static constexpr VkImageUsageFlags IMAGE_USAGE = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // An image out of the pool, which goes back into it when this is released.
    class Image final : public VideoSurfaceContents {
    public:
        virtual ~Image() override;

        VkImage image() const { return m_image; }
        NonnullRefPtr<VideoSurface> surface() const { return m_surface; }

    private:
        friend class VulkanExportableImagePool;

        Image(NonnullRefPtr<VulkanExportableImagePool> pool, VkImage image, VkDeviceMemory memory, NonnullRefPtr<VideoSurface> surface)
            : m_pool(move(pool))
            , m_image(image)
            , m_memory(memory)
            , m_surface(move(surface))
        {
        }

        NonnullRefPtr<VulkanExportableImagePool> m_pool;
        VkImage m_image { VK_NULL_HANDLE };
        VkDeviceMemory m_memory { VK_NULL_HANDLE };
        NonnullRefPtr<VideoSurface> m_surface;
    };

    // Fails if the device has no DRM format modifier to create and export such images under.
    static ErrorOr<NonnullRefPtr<VulkanExportableImagePool>> create(VulkanVideoDevice const&, VkFormat, Gfx::IntSize, u32 drm_format);
    ~VulkanExportableImagePool();

    VkFormat format() const { return m_format; }
    Gfx::IntSize size() const { return m_size; }

    ErrorOr<NonnullRefPtr<Image>> take_image();

private:
    struct FreeImage {
        VkImage image { VK_NULL_HANDLE };
        VkDeviceMemory memory { VK_NULL_HANDLE };
        NonnullRefPtr<VideoSurface> surface;
    };

    VulkanExportableImagePool(VulkanVideoDevice const&, VkFormat, Gfx::IntSize, u32 drm_format, Vector<u64> drm_format_modifiers);

    ErrorOr<FreeImage> create_image();

    VulkanVideoDevice const& m_device;
    VkFormat m_format { VK_FORMAT_UNDEFINED };
    Gfx::IntSize m_size;
    u32 m_drm_format { 0 };
    Vector<u64> m_drm_format_modifiers;

    Mutex m_mutex;
    Vector<FreeImage> m_free_images;
};

}

#endif
