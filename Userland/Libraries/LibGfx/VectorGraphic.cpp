/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/VectorGraphic.h>

namespace Gfx {

void VectorGraphic::draw_into(DeprecatedPainter& painter, IntRect const& dest, AffineTransform transform) const
{
    // Apply the transform then center within destination rectangle (this ignores any translation from the transform):
    // This allows you to easily rotate or flip the image before painting.
    auto transformed_rect = transform.map(FloatRect { {}, size() });
    auto scale = min(float(dest.width()) / transformed_rect.width(), float(dest.height()) / transformed_rect.height());
    auto centered = FloatRect { {}, transformed_rect.size().scaled(scale) }.centered_within(dest.to_type<float>());
    auto view_transform = AffineTransform {}
                              .translate(centered.location())
                              .multiply(AffineTransform {}.scale(scale, scale))
                              .multiply(AffineTransform {}.translate(-transformed_rect.location()))
                              .multiply(transform);
    return draw_transformed(painter, view_transform);
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VectorGraphic::bitmap(IntSize size, AffineTransform transform) const
{
    auto bitmap = TRY(Bitmap::create(Gfx::BitmapFormat::BGRA8888, size));
    DeprecatedPainter painter { *bitmap };
    draw_into(painter, IntRect { {}, size }, transform);
    return bitmap;
}

}
