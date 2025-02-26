/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkBitmap.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>

namespace Gfx {

struct ImmutableBitmapImpl {
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    Variant<NonnullRefPtr<Gfx::Bitmap>, NonnullRefPtr<Gfx::PaintingSurface>, Empty> source;
    ColorSpace color_space;
};

int ImmutableBitmap::width() const
{
    return m_impl->sk_image->width();
}

int ImmutableBitmap::height() const
{
    return m_impl->sk_image->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

Gfx::AlphaType ImmutableBitmap::alpha_type() const
{
    return m_impl->sk_image->alphaType() == kPremul_SkAlphaType ? Gfx::AlphaType::Premultiplied : Gfx::AlphaType::Unpremultiplied;
}

SkImage const* ImmutableBitmap::sk_image() const
{
    return m_impl->sk_image.get();
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>();
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>()->get_pixel(x, y);
}

static SkAlphaType to_skia_alpha_type(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
{
    ImmutableBitmapImpl impl;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    impl.sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    impl.sk_bitmap.setImmutable();
    impl.sk_image = impl.sk_bitmap.asImage();
    impl.source = bitmap;
    impl.color_space = move(color_space);
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto source_bitmap = bitmap;
    if (source_bitmap->alpha_type() != alpha_type) {
        source_bitmap = MUST(bitmap->clone());
        source_bitmap->set_alpha_type_destructive(alpha_type);
    }

    return create(source_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> painting_surface)
{
    ImmutableBitmapImpl impl;
    impl.sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
    impl.source = painting_surface;
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}
