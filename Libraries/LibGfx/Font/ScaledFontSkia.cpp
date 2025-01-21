/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/TypefaceSkia.h>

#include <core/SkFont.h>

namespace Gfx {

SkFont ScaledFont::skia_font(float scale) const
{
    auto const& sk_typeface = as<TypefaceSkia>(*m_typeface).sk_typeface();
    auto sk_font = SkFont { sk_ref_sp(sk_typeface), pixel_size() * scale };
    sk_font.setSubpixel(true);
    return sk_font;
}

}
