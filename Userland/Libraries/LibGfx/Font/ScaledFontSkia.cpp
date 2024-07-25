/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <core/SkFont.h>

#include <LibGfx/Font/ScaledFont.h>

namespace Gfx {

SkFont ScaledFont::skia_font(float scale) const
{
    auto const& typeface = this->typeface().skia_typeface();
    return SkFont { sk_ref_sp(typeface.ptr()), pixel_size() * scale };
}

}
