/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>

namespace Gfx {

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap)
{
    return adopt_ref(*new ImmutableBitmap(move(bitmap)));
}

ImmutableBitmap::ImmutableBitmap(NonnullRefPtr<Bitmap> bitmap)
    : m_bitmap(move(bitmap))
{
}

}
