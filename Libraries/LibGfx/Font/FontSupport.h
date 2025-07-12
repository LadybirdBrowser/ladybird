/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

enum class FontFormat : u8 {
    EmbeddedOpenType,
    OpenType,
    SVG,
    TrueType,
    TrueTypeCollection,
    WOFF,
    WOFF2,
};

enum class FontTech : u8 {
    Avar2,
    ColorCbdt,
    ColorColrv0,
    ColorColrv1,
    ColorSbix,
    ColorSvg,
    FeaturesAat,
    FeaturesGraphite,
    FeaturesOpentype,
    Incremental,
    Palettes,
    Variations,
};

bool font_format_is_supported(FontFormat);
bool font_tech_is_supported(FontTech);

}
