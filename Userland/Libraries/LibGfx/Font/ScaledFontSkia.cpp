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
    auto const& sk_typeface = verify_cast<TypefaceSkia>(*m_typeface).sk_typeface();
    return SkFont { sk_ref_sp(sk_typeface), pixel_size() * scale };
}

}
