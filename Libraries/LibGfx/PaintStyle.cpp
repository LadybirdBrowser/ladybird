/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>

namespace Gfx {

CanvasPatternPaintStyle::CanvasPatternPaintStyle(RefPtr<ImmutableBitmap> image, Repetition repetition)
    : m_image(move(image))
    , m_repetition(repetition)
{
}

ErrorOr<NonnullRefPtr<CanvasPatternPaintStyle>> CanvasPatternPaintStyle::create(RefPtr<ImmutableBitmap> image, Repetition repetition)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasPatternPaintStyle(move(image), repetition));
}

RefPtr<ImmutableBitmap> CanvasPatternPaintStyle::image() const
{
    return m_image;
}

}
