/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/NeverDestroyed.h>
#include <LibGfx/Font/GlobalFontConfig.h>
#include <fontconfig/fontconfig.h>

namespace Gfx {

GlobalFontConfig::GlobalFontConfig()
{
    FcBool inited = FcInit();
    VERIFY(inited);

    m_config = FcConfigGetCurrent();
    FcConfigReference(m_config);
}

FontHintingOptions GlobalFontConfig::hinting_for_font(FlyString const& family, float pixel_size, u16 weight, u8 slope)
{
    FontHintingOptions options;

    FcPattern* pattern = FcPatternCreate();
    if (!pattern)
        return options;

    auto family_name = family.bytes_as_string_view().to_byte_string();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<FcChar8 const*>(family_name.characters()));
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, pixel_size);
    FcPatternAddInteger(pattern, FC_WEIGHT, FcWeightFromOpenType(weight));

    int slant = FC_SLANT_ROMAN;
    if (slope == 1)
        slant = FC_SLANT_ITALIC;
    else if (slope == 2)
        slant = FC_SLANT_OBLIQUE;
    FcPatternAddInteger(pattern, FC_SLANT, slant);

    FcConfigSubstitute(m_config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    // Substitute a duplicate of the query pattern in place of a matched font, so that font-target
    // rules testing family or size apply without matching against the font set.
    FcPattern* font_pattern = FcPatternDuplicate(pattern);
    if (!font_pattern) {
        FcPatternDestroy(pattern);
        return options;
    }
    FcConfigSubstituteWithPat(m_config, font_pattern, pattern, FcMatchFont);

    FcBool hinting_enabled = FcTrue;
    FcPatternGetBool(font_pattern, FC_HINTING, 0, &hinting_enabled);

    int hint_style = FC_HINT_FULL;
    FcPatternGetInteger(font_pattern, FC_HINT_STYLE, 0, &hint_style);

    if (!hinting_enabled || hint_style == FC_HINT_NONE)
        options.style = FontHintingStyle::None;
    else if (hint_style == FC_HINT_SLIGHT)
        options.style = FontHintingStyle::Slight;
    else if (hint_style == FC_HINT_MEDIUM)
        options.style = FontHintingStyle::Normal;
    else
        options.style = FontHintingStyle::Full;

    FcBool autohint = FcFalse;
    FcPatternGetBool(font_pattern, FC_AUTOHINT, 0, &autohint);
    options.force_autohinting = autohint;

    FcPatternDestroy(font_pattern);
    FcPatternDestroy(pattern);
    return options;
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
