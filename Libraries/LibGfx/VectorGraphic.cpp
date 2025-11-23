/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/VectorGraphic.h>

namespace Gfx {

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VectorGraphic::bitmap(IntSize size, AffineTransform transform) const
{
    auto bitmap = TRY(Bitmap::create(Gfx::BitmapFormat::BGRA8888, size));
    auto painter = PainterSkia::create(bitmap);

    // Apply the transform then center within destination rectangle (this ignores any translation from the transform):
    // This allows you to easily rotate or flip the image before painting.
    auto transformed_rect = transform.map(FloatRect { {}, this->size() });
    auto scale = min(float(size.width()) / transformed_rect.width(), float(size.height()) / transformed_rect.height());
    auto centered = FloatRect { {}, transformed_rect.size().scaled(scale) }.centered_within(IntRect { {}, size }.to_type<float>());
    auto view_transform = AffineTransform {}
                              .translate(centered.location())
                              .multiply(AffineTransform {}.scale(scale, scale))
                              .multiply(AffineTransform {}.translate(-transformed_rect.location()))
                              .multiply(transform);

    painter->set_transform(view_transform);
    draw(*painter);

    return bitmap;
}

}
