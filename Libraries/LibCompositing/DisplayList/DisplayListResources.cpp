/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibGfx/Font/Font.h>

namespace Compositing {

DisplayListFontDescription DisplayListFontDescription::of(Gfx::Font const& font)
{
    return {
        .id = FontResourceId { font.id() },
        .typeface_id = font.typeface().id(),
        .point_size = font.point_size(),
        .variation_settings = font.variation_settings(),
        .synthesis = font.synthesis(),
        .is_invisible = font.is_invisible(),
    };
}

}
