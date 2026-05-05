/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SkiaUtils.h>
#include <LibIPC/File.h>

#include <core/SkColorSpace.h>
#include <core/SkSurface.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#elif defined(USE_VULKAN_DMABUF_IMAGES)
#    include <gpu/ganesh/vk/GrVkBackendSurface.h>
#    include <gpu/ganesh/vk/GrVkTypes.h>
#endif

namespace Gfx {

struct PaintingSurface::Impl {
    RefPtr<SkiaBackendContext> context;
    IntSize size;
    sk_sp<SkSurface> surface;
    RefPtr<Bitmap> bitmap;
};

#if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
static GrSurfaceOrigin origin_to_sk_origin(PaintingSurface::Origin origin)
{
    switch (origin) {
    case PaintingSurface::Origin::BottomLeft:
        return kBottomLeft_GrSurfaceOrigin;
    default:
        VERIFY_NOT_REACHED();
    case PaintingSurface::Origin::TopLeft:
        return kTopLeft_GrSurfaceOrigin;
    }
}
#endif

static sk_sp<SkColorSpace> to_skia_color_space(BitmapColorSpace color_space)
{
    switch (color_space) {
    case BitmapColorSpace::Linear:
        return SkColorSpace::MakeSRGBLinear();
    case BitmapColorSpace::SRGB:
        return SkColorSpace::MakeSRGB();
    }
    VERIFY_NOT_REACHED();
}

static sk_sp<SkColorSpace> to_skia_color_space(ColorSpace const& color_space, BitmapColorSpace fallback_color_space)
{
    if (!color_space.is_valid())
        return to_skia_color_space(fallback_color_space);
    return color_space.color_space<sk_sp<SkColorSpace>>();
}

static sk_sp<SkColorSpace> to_skia_color_space(SharedImage const& shared_image)
{
    return to_skia_color_space(shared_image.color_space(), shared_image.info().color_space);
}

#ifdef USE_VULKAN_DMABUF_IMAGES
static SkColorType vk_format_to_sk_color_type(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return kBGRA_8888_SkColorType;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return kRGBA_8888_SkColorType;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return kRGBA_F16_SkColorType;
    default:
        VERIFY_NOT_REACHED();
        return kUnknown_SkColorType;
    }
}

static VkFormat bitmap_format_to_vk_format(BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::Gray8:
    case BitmapFormat::Alpha8:
    case BitmapFormat::RGB565:
    case BitmapFormat::RGBA5551:
    case BitmapFormat::RGBA4444:
    case BitmapFormat::RGB888:
        VERIFY_NOT_REACHED();
    case BitmapFormat::BGRA8888:
    case BitmapFormat::BGRx8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case BitmapFormat::RGBA8888:
    case BitmapFormat::RGBx8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case BitmapFormat::RGBAF16:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    VERIFY_NOT_REACHED();
}

static void release_vulkan_image(void* context)
{
    VulkanImage* image = static_cast<VulkanImage*>(context);
    image->unref();
}
#endif

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(IntSize size, BitmapFormat color_type, BitmapAlpha alpha_type)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = to_skia_alpha_type(color_type, alpha_type);
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type, SkColorSpace::MakeSRGB());

    auto bitmap = Bitmap::create(color_type, alpha_type, size).value();
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap->begin(), bitmap->pitch());
    VERIFY(surface);
    return adopt_ref(*new PaintingSurface(make<Impl>(RefPtr<SkiaBackendContext> {}, size, surface, bitmap)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(RefPtr<SkiaBackendContext> context, IntSize size, BitmapFormat color_type, BitmapAlpha alpha_type)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = to_skia_alpha_type(color_type, alpha_type);
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type, SkColorSpace::MakeSRGB());

    if (context) {
        Threading::MutexLocker locker(context->m_mutex);
        auto surface = SkSurfaces::RenderTarget(context->sk_context(), skgpu::Budgeted::kNo, image_info);
        if (surface)
            return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
        dbgln("Unable to create GPU surface for size {}x{}, falling back to CPU", size.width(), size.height());
        context = nullptr;
    }

    auto bitmap = Bitmap::create(color_type, alpha_type, size).value();
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap->begin(), bitmap->pitch());
    VERIFY(surface);
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, bitmap)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_bitmap(Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto size = bitmap.size();
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.pitch());
    return adopt_ref(*new PaintingSurface(make<Impl>(RefPtr<SkiaBackendContext> {}, size, surface, bitmap)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_bitmap(Bitmap& bitmap, ColorSpace const& color_space, BitmapColorSpace fallback_color_space)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto size = bitmap.size();
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, to_skia_color_space(color_space, fallback_color_space));
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.pitch());
    return adopt_ref(*new PaintingSurface(make<Impl>(RefPtr<SkiaBackendContext> {}, size, surface, bitmap)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::create_from_shared_image(SharedImage& shared_image, NonnullRefPtr<SkiaBackendContext> context, Origin origin)
{
#ifdef AK_OS_MACOS
    if (shared_image.m_backing_kind == SharedImage::BackingKind::ShareableBitmap)
        return wrap_bitmap(*shared_image.bitmap(), shared_image.color_space(), shared_image.info().color_space);

    Threading::MutexLocker locker(context->m_mutex);

    auto bitmap = shared_image.bitmap();
    IntSize const size = shared_image.info().size;
    auto metal_texture = context->metal_context().create_texture_from_iosurface(shared_image.platform_surface_handle(), size, bitmap->format());
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_render_target = GrBackendRenderTargets::MakeMtl(metal_texture->width(), metal_texture->height(), mtl_info);
    auto surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), backend_render_target, origin_to_sk_origin(origin), to_skia_color_type(bitmap->format()), to_skia_color_space(shared_image), nullptr);
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    if (shared_image.m_backing_kind == SharedImage::BackingKind::ShareableBitmap)
        return wrap_bitmap(*shared_image.bitmap(), shared_image.color_space(), shared_image.info().color_space);

    if (!shared_image.m_vulkan_image && shared_image.m_backing_kind == SharedImage::BackingKind::LinuxDmaBuf) {
        auto const& dma_buf_payload = shared_image.linux_dma_buf_payload();
        auto fd = MUST(IPC::File::clone_fd(dma_buf_payload.file.fd())).take_fd();
        DmaBufPlaneLayout plane {
            .stride = dma_buf_payload.stride,
            .offset = dma_buf_payload.offset,
        };
        shared_image.m_vulkan_image = MUST(import_vulkan_image_from_dma_buf(context->vulkan_context(), shared_image.info().size, bitmap_format_to_vk_format(shared_image.info().pixel_format), DRM_FORMAT_MOD_LINEAR, plane, fd));
        shared_image.m_backing_kind = SharedImage::BackingKind::VulkanImage;
    }

    VERIFY(shared_image.m_vulkan_image);

    NonnullRefPtr<VulkanImage> vulkan_image = *shared_image.m_vulkan_image;
    Threading::MutexLocker locker(context->m_mutex);

    IntSize size(vulkan_image->info.extent.width, vulkan_image->info.extent.height);
    GrVkImageInfo info = {
        .fImage = vulkan_image->image,
        .fAlloc = {},
        .fImageTiling = vulkan_image->info.tiling,
        .fImageLayout = vulkan_image->info.layout,
        .fFormat = vulkan_image->info.format,
        .fImageUsageFlags = vulkan_image->info.usage,
        .fSampleCount = 1,
        .fLevelCount = 1,
        .fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        .fProtected = skgpu::Protected::kNo,
        .fYcbcrConversionInfo = {},
        .fSharingMode = vulkan_image->info.sharing_mode,
    };
    GrBackendRenderTarget render_target = GrBackendRenderTargets::MakeVk(size.width(), size.height(), info);
    vulkan_image->ref();
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), render_target, origin_to_sk_origin(origin), vk_format_to_sk_color_type(vulkan_image->info.format),
        to_skia_color_space(shared_image), nullptr, release_vulkan_image, vulkan_image.ptr());
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
#else
    (void)context;
    (void)origin;
    return wrap_bitmap(*shared_image.bitmap(), shared_image.color_space(), shared_image.info().color_space);
#endif
}

PaintingSurface::PaintingSurface(NonnullOwnPtr<Impl>&& impl)
    : m_impl(move(impl))
{
}

PaintingSurface::~PaintingSurface()
{
    m_impl->surface = nullptr;
}

NonnullRefPtr<Bitmap> PaintingSurface::snapshot_bitmap() const
{
    auto bitmap = MUST(Bitmap::create(BitmapFormat::BGRA8888, BitmapAlpha::Premultiplied, size()));
    read_into_bitmap(*bitmap);
    return bitmap;
}

void PaintingSurface::read_into_bitmap(Bitmap& bitmap) const
{
    lock_context();
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->readPixels(pixmap, 0, 0);
    unlock_context();
}

void PaintingSurface::write_from_bitmap(Bitmap const& bitmap)
{
    lock_context();
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->writePixels(pixmap, 0, 0);
    unlock_context();
}

IntSize PaintingSurface::size() const
{
    return m_impl->size;
}

IntRect PaintingSurface::rect() const
{
    return { {}, m_impl->size };
}

SkCanvas& PaintingSurface::canvas() const
{
    return *m_impl->surface->getCanvas();
}

SkSurface& PaintingSurface::sk_surface() const
{
    return *m_impl->surface;
}

void PaintingSurface::notify_content_will_change()
{
    lock_context();
    m_impl->surface->notifyContentWillChange(SkSurface::kDiscard_ContentChangeMode);
    unlock_context();
}

template<>
sk_sp<SkImage> PaintingSurface::sk_image_snapshot() const
{
    return m_impl->surface->makeImageSnapshot();
}

RefPtr<SkiaBackendContext> PaintingSurface::skia_backend_context() const
{
    return m_impl->context;
}

void PaintingSurface::flush()
{
    if (on_flush)
        on_flush(*this);
}

void PaintingSurface::lock_context() const
{
    auto& context = m_impl->context;
    if (context)
        context->m_mutex.lock();
}

void PaintingSurface::unlock_context() const
{
    auto& context = m_impl->context;
    if (context)
        context->m_mutex.unlock();
}

}
