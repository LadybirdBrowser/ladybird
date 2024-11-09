/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>

namespace Gfx {

static size_t s_next_immutable_bitmap_id = 0;

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap)
{
    return adopt_ref(*new ImmutableBitmap(move(bitmap)));
}

ImmutableBitmap::ImmutableBitmap(NonnullRefPtr<Bitmap> bitmap)
    : m_bitmap(move(bitmap))
    , m_id(s_next_immutable_bitmap_id++)
{
}

}
