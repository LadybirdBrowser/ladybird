/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/LsanSuppressions.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Font/GlobalFontConfig.h>
#include <fontconfig/fontconfig.h>

namespace Gfx {

int fontconfig_slant(u8 slope)
{
    if (slope == 1)
        return FC_SLANT_ITALIC;
    if (slope == 2)
        return FC_SLANT_OBLIQUE;
    return FC_SLANT_ROMAN;
}

GlobalFontConfig::GlobalFontConfig()
{
    FcBool inited = FcInit();
    VERIFY(inited);

    m_config = FcConfigGetCurrent();
    FcConfigReference(m_config);
}

static FcPattern* resolve_rendering_pattern(FcConfig* config, FcPattern* query, FcChar8 const* font_family = nullptr)
{
    FcConfigSubstitute(config, query, FcMatchPattern);
    // The display-wide lookup has no font size or style for a rule to match.
    if (font_family)
        FcDefaultSubstitute(query);

    // Substitute a duplicate of the query in place of a matched font, so that font-target rules apply without
    // matching against the font set.
    auto* pattern = FcPatternDuplicate(query);
    if (!pattern)
        return nullptr;

    // Pattern substitution can add default families even when querying the display's pixel order. Only a
    // selected font should match a font-target family rule.
    FcPatternDel(pattern, FC_FAMILY);
    if (font_family && !FcPatternAddString(pattern, FC_FAMILY, font_family)) {
        FcPatternDestroy(pattern);
        return nullptr;
    }

    FcConfigSubstituteWithPat(config, pattern, query, FcMatchFont);
    return pattern;
}

static FontSubpixelOrder subpixel_order_of(FcPattern* pattern)
{
    FcBool antialias = FcTrue;
    FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &antialias);
    int rgba = FC_RGBA_UNKNOWN;
    if (!antialias || FcPatternGetInteger(pattern, FC_RGBA, 0, &rgba) != FcResultMatch)
        return FontSubpixelOrder::None;

    switch (rgba) {
    case FC_RGBA_RGB:
        return FontSubpixelOrder::RGB;
    case FC_RGBA_BGR:
        return FontSubpixelOrder::BGR;
    case FC_RGBA_VRGB:
        return FontSubpixelOrder::VerticalRGB;
    case FC_RGBA_VBGR:
        return FontSubpixelOrder::VerticalBGR;
    default:
        return FontSubpixelOrder::None;
    }
}

FontRenderingOptions GlobalFontConfig::rendering_options_for_font(FlyString const& family, float pixel_size, u16 weight, u8 slope)
{
    FontRenderingOptions options;

    FcPattern* pattern = FcPatternCreate();
    if (!pattern)
        return options;

    auto family_name = family.bytes_as_string_view().to_byte_string();
    auto const* font_family = reinterpret_cast<FcChar8 const*>(family_name.characters());
    FcPatternAddString(pattern, FC_FAMILY, font_family);
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, pixel_size);
    FcPatternAddInteger(pattern, FC_WEIGHT, FcWeightFromOpenType(weight));

    FcPatternAddInteger(pattern, FC_SLANT, fontconfig_slant(slope));

    auto* font_pattern = resolve_rendering_pattern(m_config, pattern, font_family);
    FcPatternDestroy(pattern);
    if (!font_pattern)
        return options;

    FcBool hinting_enabled = FcTrue;
    FcPatternGetBool(font_pattern, FC_HINTING, 0, &hinting_enabled);

    int hint_style = FC_HINT_FULL;
    FcPatternGetInteger(font_pattern, FC_HINT_STYLE, 0, &hint_style);

    if (!hinting_enabled || hint_style == FC_HINT_NONE)
        options.hinting_style = FontHintingStyle::None;
    else if (hint_style == FC_HINT_SLIGHT)
        options.hinting_style = FontHintingStyle::Slight;
    else if (hint_style == FC_HINT_MEDIUM)
        options.hinting_style = FontHintingStyle::Normal;
    else
        options.hinting_style = FontHintingStyle::Full;

    FcBool autohint = FcFalse;
    FcPatternGetBool(font_pattern, FC_AUTOHINT, 0, &autohint);
    options.force_autohinting = autohint;
    options.allows_lcd_text = subpixel_order_of(font_pattern) != FontSubpixelOrder::None;

    FcPatternDestroy(font_pattern);
    return options;
}

FontSubpixelOrder GlobalFontConfig::subpixel_order() const
{
    auto* query = FcPatternCreate();
    if (!query)
        return FontSubpixelOrder::None;
    auto* pattern = resolve_rendering_pattern(m_config, query);
    FcPatternDestroy(query);
    if (!pattern)
        return FontSubpixelOrder::None;
    ScopeGuard destroy_pattern = [&] { FcPatternDestroy(pattern); };

    return subpixel_order_of(pattern);
}

GlobalFontConfig::~GlobalFontConfig()
{
    FcConfigDestroy(m_config);
}

GlobalFontConfig& GlobalFontConfig::the()
{
    static NeverDestroyed<GlobalFontConfig> s_the;
    return *s_the;
}

FcConfig* GlobalFontConfig::get()
{
    return m_config;
}

}
