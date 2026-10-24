/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaUtils.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkPaint.h>
#include <core/SkRect.h>
#include <core/SkSurface.h>
#include <core/SkSurfaceProps.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#elif defined(USE_VULKAN_DMABUF_IMAGES)
#    include <LibGfx/VulkanImage.h>
#    include <gpu/ganesh/vk/GrVkBackendSurface.h>
#    include <gpu/ganesh/vk/GrVkTypes.h>
#elif defined(USE_DIRECTX)
#    include <AK/Windows.h>
#    include <LibGfx/D3DSharedTexture.h>
#    include <gpu/ganesh/d3d/GrD3DBackendSurface.h>
#    include <gpu/ganesh/d3d/GrD3DTypes.h>
#endif

namespace Gfx {

struct PaintingSurface::Impl {
    AK_ALLOC_WITH_KMALLOC;

    RefPtr<SkiaBackendContext> context;
    IntSize size;
    sk_sp<SkSurface> surface;
    RefPtr<Bitmap> bitmap;
    bool wraps_shared_image { false };
};

#if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_DIRECTX)
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

#ifdef USE_VULKAN_DMABUF_IMAGES
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
        SkColorSpace::MakeSRGB(), nullptr, release_vulkan_image, vulkan_image.ptr());
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr, true)));
}
#endif

#ifdef USE_DIRECTX
static void release_d3d_texture(void* context)
{
    auto* texture = static_cast<D3DSharedTexture*>(context);
    texture->unref();
}

ErrorOr<NonnullRefPtr<PaintingSurface>> PaintingSurface::create_from_d3d_texture(NonnullRefPtr<SkiaBackendContext> context, NonnullRefPtr<D3DSharedTexture> texture, Origin origin)
{
    auto size = texture->size();
    GrD3DTextureResourceInfo info(nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON, DXGI_FORMAT_R8G8B8A8_UNORM,
        /* sampleCount */ 1, /* levelCount */ 1, /* sampleQualityLevel */ 0);
    // NOTE: The positional resource argument of GrD3DTextureResourceInfo would adopt our reference, so attach it with retain() instead.
    info.fResource.retain(texture->resource());
    auto render_target = GrBackendRenderTargets::MakeD3D(size.width(), size.height(), info);
    // Note, we're implicitly giving Skia a reference to the texture. It will eventually be released by the callback function,
    // which is guaranteed to be invoked even if wrapping fails.
    texture->ref();
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), render_target, origin_to_sk_origin(origin),
        kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr, release_d3d_texture, texture.ptr());
    if (!surface)
        return Error::from_string_literal("Failed to wrap shared Direct3D texture in a Skia surface");
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr, true)));
}
#endif

// The Skia color space of pixels in the color space, where sRGB is explicit.
static sk_sp<SkColorSpace> skia_color_space_of_pixels(ColorSpace const& color_space)
{
    if (auto skia_color_space = to_skia_color_space(color_space))
        return skia_color_space;
    return SkColorSpace::MakeSRGB();
}

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(IntSize size, BitmapFormat color_type, AlphaType alpha_type, RefPtr<SkiaBackendContext> context, ColorSpace const& color_space)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = to_skia_alpha_type(color_type, alpha_type);
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type, skia_color_space_of_pixels(color_space));

    if (context) {
        // NB: Dynamic MSAA lets Skia render complex antialiased paths on the GPU instead of
        //     rasterizing coverage masks on the CPU and uploading them as textures.
        SkSurfaceProps surface_properties { SkSurfaceProps::kDynamicMSAA_Flag, kUnknown_SkPixelGeometry };
        auto surface = SkSurfaces::RenderTarget(context->sk_context(), skgpu::Budgeted::kNo, image_info, 0, &surface_properties);
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

#ifdef AK_OS_MACOS
NonnullRefPtr<PaintingSurface> PaintingSurface::create_from_shared_image_buffer(SharedImageBuffer& shared_image_buffer, NonnullRefPtr<SkiaBackendContext> context, Origin origin)
{
    auto const& iosurface_handle = shared_image_buffer.iosurface_handle();
    auto metal_texture = context->metal_context().create_texture_from_iosurface(iosurface_handle, MetalTextureFormat::BGRA8, 0);
    IntSize const size { metal_texture->width(), metal_texture->height() };
    auto image_info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_render_target = GrBackendRenderTargets::MakeMtl(metal_texture->width(), metal_texture->height(), mtl_info);
    auto surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), backend_render_target, origin_to_sk_origin(origin), kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    return adopt_ref(*new PaintingSurface(make<Impl>(context, size, surface, nullptr, true)));
}
#endif

PaintingSurface::PaintingSurface(NonnullOwnPtr<Impl>&& impl)
    : m_impl(move(impl))
{
}

PaintingSurface::~PaintingSurface()
{
    m_impl->surface = nullptr;
}

void PaintingSurface::read_into_bitmap(Bitmap& bitmap, IntPoint source_position, ColorSpace const& color_space) const
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, skia_color_space_of_pixels(color_space));
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());

    auto surface_color_space = m_impl->surface->imageInfo().refColorSpace();
    if (SkColorSpace::Equals(surface_color_space.get(), image_info.colorSpace())) {
        m_impl->surface->readPixels(pixmap, source_position.x(), source_position.y());
        return;
    }

    // NB: Drawing clamps each premultiplied component to [0, 1], but not to the alpha, so a translucent color that a
    //     draw converted from a wider gamut can have more color than its alpha allows. The pixels are unpremultiplied
    //     in the color space of the surface first, and clamped to its gamut, which holds the color that they show, so
    //     that the conversion to the other color space starts from that. They are unpremultiplied to floats, which
    //     loses no precision before the conversion, a strip of rows at a time, so that a large readback needs no
    //     large buffer of floats.
    if (bitmap.width() <= 0 || bitmap.height() <= 0)
        return;
    static constexpr int max_strip_pixel_count = 1024 * 1024;
    auto strip_height = clamp(max_strip_pixel_count / bitmap.width(), 1, bitmap.height());
    auto strip_info = SkImageInfo::Make(bitmap.width(), strip_height, kRGBA_F32_SkColorType, kUnpremul_SkAlphaType, surface_color_space);
    auto strip_pixels = ByteBuffer::create_uninitialized(strip_info.computeMinByteSize());
    if (strip_pixels.is_error())
        return;
    for (int y = 0; y < bitmap.height(); y += strip_height) {
        auto rows = min(strip_height, bitmap.height() - y);
        SkPixmap const unpremultiplied_pixmap(strip_info.makeWH(bitmap.width(), rows), strip_pixels.value().data(), strip_info.minRowBytes());
        if (!m_impl->surface->readPixels(unpremultiplied_pixmap, source_position.x(), source_position.y() + y))
            return;
        auto components = Span<float> { reinterpret_cast<float*>(strip_pixels.value().data()), unpremultiplied_pixmap.computeByteSize() / sizeof(float) };
        for (auto& component : components)
            component = clamp(component, 0.0f, 1.0f);
        SkPixmap destination_rows;
        if (!pixmap.extractSubset(&destination_rows, SkIRect::MakeXYWH(0, y, bitmap.width(), rows)))
            return;
        unpremultiplied_pixmap.readPixels(destination_rows);
    }
}

void PaintingSurface::write_from_bitmap(Bitmap const& bitmap, IntPoint destination_position, ColorSpace const& color_space)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = to_skia_alpha_type(bitmap.format(), bitmap.alpha_type());
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, skia_color_space_of_pixels(color_space));
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->writePixels(pixmap, destination_position.x(), destination_position.y());
}

void PaintingSurface::copy_from_surface(PaintingSurface& source)
{
    source.flush();

    // OPTIMIZATION: Pixels in memory are written straight into this surface, which for a surface on the GPU is one
    //               upload. A snapshot of them would copy them first, and the GPU would upload the copy into a texture
    //               of its own before drawing it here.
    SkPixmap source_pixels;
    if (source.size() == size() && source.m_impl->surface->peekPixels(&source_pixels)) {
        m_impl->surface->writePixels(source_pixels, 0, 0);
        return;
    }

    auto image = source.m_impl->surface->makeImageSnapshot();
    if (!image)
        return;

    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas().drawImageRect(
        image.get(),
        SkRect::MakeIWH(image->width(), image->height()),
        SkRect::MakeIWH(size().width(), size().height()),
        SkSamplingOptions {},
        &paint,
        SkCanvas::kStrict_SrcRectConstraint);
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
    m_impl->surface->notifyContentWillChange(SkSurface::kDiscard_ContentChangeMode);
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

bool PaintingSurface::wraps_shared_image() const
{
    return m_impl->wraps_shared_image;
}

void PaintingSurface::flush()
{
    if (on_flush)
        on_flush(*this);
}

}
