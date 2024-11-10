/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkColorSpace.h>
#include <core/SkSurface.h>
#include <gpu/GrBackendSurface.h>
#include <gpu/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#endif

namespace Gfx {

struct PaintingSurface::Impl {
    IntSize size;
    sk_sp<SkSurface> surface;
    RefPtr<Bitmap> bitmap;
    RefPtr<SkiaBackendContext> context;
};

NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(RefPtr<SkiaBackendContext> context, Gfx::IntSize size, Gfx::BitmapFormat color_type, Gfx::AlphaType alpha_type)
{
    auto sk_color_type = to_skia_color_type(color_type);
    auto sk_alpha_type = alpha_type == Gfx::AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto image_info = SkImageInfo::Make(size.width(), size.height(), sk_color_type, sk_alpha_type);

    if (!context) {
        auto bitmap = Gfx::Bitmap::create(color_type, alpha_type, size).value();
        auto surface = SkSurfaces::WrapPixels(image_info, bitmap->begin(), bitmap->pitch());
        VERIFY(surface);
        return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, bitmap, context)));
    }

    auto surface = SkSurfaces::RenderTarget(context->sk_context(), skgpu::Budgeted::kNo, image_info);
    VERIFY(surface);
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, nullptr, context)));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_bitmap(Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == Gfx::AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto size = bitmap.size();
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type);
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.pitch());
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, bitmap, nullptr)));
}

#ifdef AK_OS_MACOS
NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_metal_surface(Gfx::MetalTexture& metal_texture, RefPtr<SkiaBackendContext> context)
{
    IntSize const size { metal_texture.width(), metal_texture.height() };
    auto image_info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture.texture());
    auto backend_render_target = GrBackendRenderTargets::MakeMtl(metal_texture.width(), metal_texture.height(), mtl_info);
    auto surface = SkSurfaces::WrapBackendRenderTarget(context->sk_context(), backend_render_target, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, nullptr, nullptr);
    return adopt_ref(*new PaintingSurface(make<Impl>(size, surface, nullptr, context)));
}
#endif

PaintingSurface::PaintingSurface(NonnullOwnPtr<Impl>&& impl)
    : m_impl(move(impl))
{
}

PaintingSurface::~PaintingSurface() = default;

void PaintingSurface::read_into_bitmap(Gfx::Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == Gfx::AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type);
    SkPixmap const pixmap(image_info, bitmap.begin(), bitmap.pitch());
    m_impl->surface->readPixels(pixmap, 0, 0);
}

void PaintingSurface::write_from_bitmap(Gfx::Bitmap& bitmap)
{
    auto color_type = to_skia_color_type(bitmap.format());
    auto alpha_type = bitmap.alpha_type() == Gfx::AlphaType::Premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, alpha_type);
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

template<>
sk_sp<SkImage> PaintingSurface::sk_image_snapshot() const
{
    return m_impl->surface->makeImageSnapshot();
}

void PaintingSurface::flush() const
{
    if (auto context = m_impl->context) {
        context->flush_and_submit(m_impl->surface.get());
    }
}

}
