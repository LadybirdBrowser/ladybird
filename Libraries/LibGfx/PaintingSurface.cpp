/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkColorSpace.h>
#include <core/SkSurface.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#elif defined(USE_VULKAN_IMAGES)
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

#if defined(AK_OS_MACOS) || defined(USE_VULKAN_IMAGES)
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

#ifdef USE_VULKAN_IMAGES
static SkColorType vk_format_to_sk_color_type(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return kBGRA_8888_SkColorType;
    // add more as needed
    default:
        VERIFY_NOT_REACHED();
        return kUnknown_SkColorType;
    }
}

static void release_vulkan_image(void* context)
{
    VulkanImage* image = static_cast<VulkanImage*>(context);
    image->unref();
}

NonnullRefPtr<PaintingSurface> PaintingSurface::create_from_vkimage(NonnullRefPtr<SkiaBackendContext> context, NonnullRefPtr<VulkanImage> vulkan_image, Origin origin)
{
    context->lock();
    ScopeGuard unlock_guard([&context] {
        context->unlock();
    });

    IntSize size(vulkan_image->info.extent.width, vulkan_image->info.extent.height);
    GrVkImageInfo info = {
        .fImage = vulkan_image->image,
        .fAlloc = {}, // we're managing the memory ourselves
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
    GrBackendRenderTarget rt = GrBackendRenderTargets::MakeVk(size.width(), size.height(), info);
    // Note, we're implicitly giving Skia a reference to vulkan_image. It will eventually be released by the callback function.
    vulkan_image->ref();
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), rt, origin_to_sk_origin(origin), vk_format_to_sk_color_type(vulkan_image->info.format),
        nullptr, nullptr, release_vulkan_image, vulkan_image.ptr());
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
}
#endif

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(RefPtr<SkiaBackendContext> context, IntSize size, BitmapFormat color_type, AlphaType alpha_type)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = to_skia_alpha_type(color_type, alpha_type);
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type, SkColorSpace::MakeSRGB());

    if (!context) {
        auto bitmap = Bitmap::create(color_type, alpha_type, size).value();
        auto surface = SkSurfaces::WrapPixels(image_info, bitmap->begin(), bitmap->pitch());
        VERIFY(surface);
        return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, bitmap)));
    }

    context->lock();
    auto surface = SkSurfaces::RenderTarget(context->sk_context(), skgpu::Budgeted::kNo, image_info);
    VERIFY(surface);
    context->unlock();
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
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

#ifdef AK_OS_MACOS
NonnullRefPtr<PaintingSurface> PaintingSurface::create_from_iosurface(Core::IOSurfaceHandle&& iosurface_handle, NonnullRefPtr<SkiaBackendContext> context, Origin origin)
{
    context->lock();
    ScopeGuard unlock_guard([&context] {
        context->unlock();
    });

    auto metal_texture = context->metal_context().create_texture_from_iosurface(iosurface_handle);
    IntSize const size { metal_texture->width(), metal_texture->height() };
    auto image_info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_render_target = GrBackendRenderTargets::MakeMtl(metal_texture->width(), metal_texture->height(), mtl_info);
    auto surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), backend_render_target, origin_to_sk_origin(origin), kBGRA_8888_SkColorType, nullptr, nullptr);
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr)));
}
#endif

PaintingSurface::PaintingSurface(NonnullOwnPtr<Impl>&& impl)
    : m_impl(move(impl))
{
}

PaintingSurface::~PaintingSurface()
{
    lock_context();
    m_impl->surface = nullptr;
    unlock_context();
}

void PaintingSurface::read_into_bitmap(Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->readPixels(pixmap, 0, 0);
}

void PaintingSurface::write_from_bitmap(Bitmap const& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->writePixels(pixmap, 0, 0);
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

void PaintingSurface::flush()
{
    if (on_flush)
        on_flush(*this);
}

void PaintingSurface::lock_context() const
{
    auto& context = m_impl->context;
    if (context)
        context->lock();
}

void PaintingSurface::unlock_context() const
{
    auto& context = m_impl->context;
    if (context)
        context->unlock();
}

}
