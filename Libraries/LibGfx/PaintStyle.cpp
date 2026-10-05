/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintStyle.h>

namespace Gfx {

// NB: Keep canvas paint-style factories in LibGfx so the objects use the same RTTI
//     as the dynamic casts in canvas command conversion. Inlined factories in
//     libraries with hidden visibility can produce separate copies of the RTTI.
ErrorOr<NonnullRefPtr<SolidColorPaintStyle>> SolidColorPaintStyle::create(Color color)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) SolidColorPaintStyle(color));
}

CanvasPatternPaintStyle::CanvasPatternPaintStyle(Optional<DecodedImageFrame> image, Repetition repetition)
    : m_image(move(image))
    , m_repetition(repetition)
{
}

ErrorOr<NonnullRefPtr<CanvasPatternPaintStyle>> CanvasPatternPaintStyle::create(Optional<DecodedImageFrame> image, Repetition repetition)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasPatternPaintStyle(move(image), repetition));
}

Optional<DecodedImageFrame> CanvasPatternPaintStyle::image() const
{
    return m_image;
}

ErrorOr<NonnullRefPtr<CanvasLinearGradientPaintStyle>> CanvasLinearGradientPaintStyle::create(FloatPoint p0, FloatPoint p1)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasLinearGradientPaintStyle(p0, p1));
}

ErrorOr<NonnullRefPtr<CanvasConicGradientPaintStyle>> CanvasConicGradientPaintStyle::create(FloatPoint center, float start_angle)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasConicGradientPaintStyle(center, start_angle));
}

ErrorOr<NonnullRefPtr<CanvasRadialGradientPaintStyle>> CanvasRadialGradientPaintStyle::create(FloatPoint start_center, float start_radius, FloatPoint end_center, float end_radius)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasRadialGradientPaintStyle(start_center, start_radius, end_center, end_radius));
}

}
