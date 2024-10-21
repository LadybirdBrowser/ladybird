/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>

namespace Gfx {

static size_t s_next_immutable_bitmap_id = 0;

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
{
    return adopt_ref(*new ImmutableBitmap(move(bitmap), move(color_space)));
}

ImmutableBitmap::ImmutableBitmap(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
    : m_bitmap(move(bitmap))
    , m_color_space(move(color_space))
    , m_id(s_next_immutable_bitmap_id++)
{
}

}
