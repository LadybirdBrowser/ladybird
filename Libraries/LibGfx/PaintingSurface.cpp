/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkColorSpace.h>
#include <core/SkSurface.h>
#include <gpu/GrBackendSurface.h>
#include <gpu/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#endif

namespace Gfx {

struct PaintingSurface::Impl {
    IntSize size;
    sk_sp<SkSurface> surface;
    RefPtr<Bitmap> bitmap;
};

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(RefPtr<SkiaBackendContext> context, IntSize size, BitmapFormat color_type, AlphaType alpha_type)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = alpha_type == AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type, SkColorSpace::MakeSRGB());

    if (!context) {
        auto bitmap = Bitmap::create(color_type, alpha_type, size).value();
        auto surface = SkSurfaces::WrapPixels(image_info, bitmap->begin(), bitmap->pitch());
        VERIFY(surface);
        return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, bitmap)));
    }

    auto surface = SkSurfaces::RenderTarget(context->sk_context(), skgpu::Budgeted::kNo, image_info);
    VERIFY(surface);
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, nullptr)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_bitmap(Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto size = bitmap.size();
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.pitch());
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, bitmap)));
}

#ifdef AK_OS_MACOS
NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_iosurface(Core::IOSurfaceHandle const& iosurface_handle, RefPtr<SkiaBackendContext> context, Origin origin)
{
    auto metal_texture = context->metal_context().create_texture_from_iosurface(iosurface_handle);
    IntSize const size { metal_texture->width(), metal_texture->height() };
    auto image_info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_render_target = GrBackendRenderTargets::MakeMtl(metal_texture->width(), metal_texture->height(), mtl_info);
    GrSurfaceOrigin sk_origin;
    switch (origin) {
    case Origin::TopLeft:
        sk_origin = kTopLeft_GrSurfaceOrigin;
        break;
    case Origin::BottomLeft:
        sk_origin = kBottomLeft_GrSurfaceOrigin;
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    auto surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), backend_render_target, sk_origin, kBGRA_8888_SkColorType, nullptr, nullptr);
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, nullptr)));
}
#endif

PaintingSurface::PaintingSurface(NonnullOwnPtr<Impl>&& impl)
    : m_impl(move(impl))
{
}

PaintingSurface::~PaintingSurface() = default;

void PaintingSurface::read_into_bitmap(Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type, SkColorSpace::MakeSRGB());
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->readPixels(pixmap, 0, 0);
}

void PaintingSurface::write_from_bitmap(Bitmap const& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
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
    m_impl->surface->notifyContentWillChange(SkSurface::kDiscard_ContentChangeMode);
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

}
