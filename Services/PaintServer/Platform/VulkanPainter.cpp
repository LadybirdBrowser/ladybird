/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/Platform/VulkanPainter.h>
#include <core/SkImage.h>
#include <sys/mman.h>

namespace PaintServer {

ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> VulkanPainter::create_gpu_backed_skia_context()
{
    auto vulkan_context = TRY(Gfx::create_vulkan_context());
    auto context = Gfx::SkiaBackendContext::create_vulkan_context(vulkan_context);
    if (!context)
        return Error::from_string_literal("Failed to create Skia Vulkan backend context");

    return context.release_nonnull();
}

VkFormat VulkanPainter::to_vk_format(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::BGRA8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case Gfx::BitmapFormat::RGBA8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Gfx::BitmapFormat::RGBAF16:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Gfx::BitmapFormat::BGRx8888:
    case Gfx::BitmapFormat::RGBx8888:
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

ErrorOr<Gfx::SharedImage> VulkanPainter::create_gpu_backed_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
#if !defined(USE_VULKAN_DMABUF_IMAGES)
    (void)image_id;
    (void)size;
    (void)format;
    return Error::from_string_literal("VulkanPainter: shared images require USE_VULKAN_DMABUF_IMAGES");
#else
    (void)image_id;
    VkFormat vk_format = to_vk_format(format);
    if (vk_format == VK_FORMAT_UNDEFINED)
        return Error::from_string_literal("VulkanPainter: unsupported pixel format");

    auto context = TRY(skia_context());
    auto const& vulkan_context = context->vulkan_context();
    NonnullRefPtr<Gfx::VulkanImage> image = TRY(Gfx::create_dma_buf_vulkan_image(vulkan_context, size, vk_format, true));

    if (image->info.row_pitch > NumericLimits<u32>::max())
        return Error::from_string_literal("VulkanPainter: shared image row pitch too large");

    Gfx::BitmapInfo bitmap_info {
        .size = size,
        .row_bytes = static_cast<u32>(image->info.row_pitch),
        .mip_level_count = 1,
        .sample_count = 1,
        .tiling_modifier = 0,
        .pixel_format = format,
        .color_space = Gfx::BitmapColorSpace::SRGB,
        .alpha_type = Gfx::BitmapAlpha::Premultiplied,
        .origin = Gfx::BitmapOrigin::TopLeft,
    };

    return Gfx::SharedImage::create_from_vulkan_image(image, bitmap_info);
#endif
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VulkanPainter::import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image)
{
    auto const* dmabuf = shared_image.linux_dma_buf_payload();
    if (!dmabuf)
        return Error::from_string_literal("VulkanPainter: expected Linux dma-buf shared image");

    auto const& info = shared_image.info();

    if (dmabuf->offset != 0)
        return Error::from_string_literal("VulkanPainter: software painting requires zero-offset dma-buf presentation buffers");

    Gfx::BitmapFormat bitmap_format;
    switch (info.pixel_format) {
    case Gfx::BitmapFormat::BGRA8888:
        bitmap_format = Gfx::BitmapFormat::BGRA8888;
        break;
    case Gfx::BitmapFormat::RGBA8888:
        bitmap_format = Gfx::BitmapFormat::RGBA8888;
        break;
    case Gfx::BitmapFormat::BGRx8888:
    case Gfx::BitmapFormat::RGBx8888:
    case Gfx::BitmapFormat::RGBAF16:
    default:
        return Error::from_string_literal("VulkanPainter: unsupported software painting pixel format");
    }

    Gfx::BitmapAlpha alpha_type = info.alpha_type;

    size_t mapped_size = Gfx::Bitmap::size_in_bytes(dmabuf->stride, info.size.height());
    void* data = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf->file.fd(), 0);
    if (data == MAP_FAILED)
        return Error::from_string_literal("VulkanPainter: failed to map dma-buf presentation buffer");

    auto bitmap_or_error = Gfx::Bitmap::create_wrapper(bitmap_format, alpha_type, info.size, dmabuf->stride, data, [data, mapped_size] {
        VERIFY(munmap(data, mapped_size) == 0);
    });
    if (bitmap_or_error.is_error()) {
        VERIFY(munmap(data, mapped_size) == 0);
        return bitmap_or_error.release_error();
    }

    return bitmap_or_error.release_value();
}

ErrorOr<Gfx::SharedImage> VulkanPainter::import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image)
{
    auto const* dmabuf = shared_image.linux_dma_buf_payload();
    if (!dmabuf)
        return Error::from_string_literal("VulkanPainter: expected Linux dma-buf shared image");

    VkFormat vk_format = to_vk_format(shared_image.info().pixel_format);
    if (vk_format == VK_FORMAT_UNDEFINED)
        return Error::from_string_literal("VulkanPainter: unsupported pixel format");

    auto context = TRY(skia_context());
    auto const& vulkan_context = context->vulkan_context();

    int fd = TRY(IPC::File::clone_fd(dmabuf->file.fd())).take_fd();
    Gfx::DmaBufPlaneLayout plane {
        .stride = dmabuf->stride,
        .offset = dmabuf->offset,
    };

    NonnullRefPtr<Gfx::VulkanImage> image = TRY(Gfx::import_vulkan_image_from_dma_buf(vulkan_context, shared_image.info().size, vk_format, DRM_FORMAT_MOD_LINEAR, plane, fd));
    return Gfx::SharedImage::create_from_vulkan_image(image, shared_image.info());
}

char const* VulkanPainter::backend_name() const
{
    return "VulkanPainter";
}

NonnullOwnPtr<Painter> Painter::create(PaintingMode painting_mode)
{
    return make<VulkanPainter>(painting_mode);
}

}
