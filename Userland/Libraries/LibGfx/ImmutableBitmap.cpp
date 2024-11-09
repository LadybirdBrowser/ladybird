/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>

#include <core/SkBitmap.h>
#include <core/SkImage.h>

namespace Gfx {

struct ImmutableBitmapImpl {
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    RefPtr<Gfx::Bitmap> gfx_bitmap;
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
    return m_impl->gfx_bitmap;
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    if (m_impl->gfx_bitmap) {
        return m_impl->gfx_bitmap->get_pixel(x, y);
    }
    VERIFY_NOT_REACHED();
}

static SkColorType to_skia_color_type(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::Invalid:
        return kUnknown_SkColorType;
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBA8888:
        return kRGBA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBx8888:
        return kRGB_888x_SkColorType;
    default:
        return kUnknown_SkColorType;
    }
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

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap)
{
    ImmutableBitmapImpl impl;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()));
    impl.sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    impl.sk_bitmap.setImmutable();
    impl.sk_image = impl.sk_bitmap.asImage();
    impl.gfx_bitmap = bitmap;
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}
