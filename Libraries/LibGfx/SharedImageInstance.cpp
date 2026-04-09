/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageInstance.h>
#include <LibGfx/VulkanImage.h>

namespace Gfx {

#ifdef AK_OS_MACOS
static constexpr auto shared_image_backing_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_backing_alpha_type = AlphaType::Premultiplied;

static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(Core::IOSurfaceHandle const& iosurface_handle)
{
    auto size = IntSize(static_cast<int>(iosurface_handle.width()), static_cast<int>(iosurface_handle.height()));
    auto bitmap_handle = Core::IOSurfaceHandle::from_mach_port(iosurface_handle.create_mach_port());
    return MUST(Bitmap::create_wrapper(shared_image_backing_format, shared_image_backing_alpha_type, size, iosurface_handle.bytes_per_row(), iosurface_handle.data(), [handle = move(bitmap_handle)] { }));
}

SharedImageInstance::SharedImageInstance(Core::IOSurfaceHandle&& iosurface_handle, NonnullRefPtr<Bitmap> bitmap)
    : m_iosurface_handle(move(iosurface_handle))
    , m_bitmap(move(bitmap))
{
}
#else
static constexpr auto shared_image_backing_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_backing_alpha_type = AlphaType::Premultiplied;

SharedImageInstance::SharedImageInstance(NonnullRefPtr<Bitmap> bitmap
#    ifdef USE_VULKAN_DMABUF_IMAGES
    ,
    RefPtr<VulkanImage> vulkan_image
#    endif
    )
    : m_bitmap(move(bitmap))
#    ifdef USE_VULKAN_DMABUF_IMAGES
    , m_vulkan_image(move(vulkan_image))
#    endif
{
}
#endif

SharedImageInstance SharedImageInstance::create(IntSize size)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageInstance(move(iosurface_handle), move(bitmap));
#else
    return SharedImageInstance(MUST(Bitmap::create_shareable(shared_image_backing_format, shared_image_backing_alpha_type, size)));
#endif
}

ErrorOr<SharedImageInstance> SharedImageInstance::import_from_payload(SharedImagePayload shared_image, [[maybe_unused]] VulkanContext const* vulkan_context)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::from_mach_port(shared_image.m_port);
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageInstance(move(iosurface_handle), move(bitmap));
#else
    if (shared_image.is_shareable_bitmap()) {
        auto shareable_bitmap = shared_image.release_shareable_bitmap();
        auto* bitmap = shareable_bitmap.bitmap();
        VERIFY(bitmap);
        return SharedImageInstance(NonnullRefPtr { *bitmap });
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    if (!vulkan_context)
        return Error::from_string_literal("missing Vulkan context for dma-buf import");

    auto linux_dma_buf = shared_image.release_linux_dma_buf();
    auto vulkan_format = TRY(drm_format_to_vk_format(linux_dma_buf.drm_format));
    auto bitmap_format = TRY(vk_format_to_bitmap_format(vulkan_format));

    auto vulkan_image = TRY(import_vulkan_image_from_dma_buf(
        *vulkan_context,
        linux_dma_buf.size,
        vulkan_format,
        linux_dma_buf.modifier,
        linux_dma_buf.plane,
        linux_dma_buf.fd.take_fd()));
    auto bitmap = TRY(Bitmap::create(bitmap_format, AlphaType::Premultiplied, linux_dma_buf.size));
    return SharedImageInstance(move(bitmap), move(vulkan_image));
#    else
    return Error::from_string_literal("received linux dma-buf payload without import support");
#    endif
#endif
}

SharedImageInstance::SharedImageInstance(SharedImageInstance&&) = default;

SharedImageInstance& SharedImageInstance::operator=(SharedImageInstance&&) = default;

SharedImageInstance::~SharedImageInstance() = default;

SharedImagePayload SharedImageInstance::export_payload() const
{
#ifdef AK_OS_MACOS
    return SharedImagePayload { m_iosurface_handle.create_mach_port() };
#else
#    ifdef USE_VULKAN_DMABUF_IMAGES
    VERIFY(!m_vulkan_image);
#    endif
    return SharedImagePayload { ShareableBitmap { m_bitmap, ShareableBitmap::ConstructWithKnownGoodBitmap } };
#endif
}

}
