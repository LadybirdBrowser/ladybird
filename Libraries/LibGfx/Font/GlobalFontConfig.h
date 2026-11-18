/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <fontconfig/fontconfig.h>

namespace AK {

template<typename T>
class NeverDestroyed;

}

namespace Gfx {

enum class FontHintingStyle {
    None,
    Slight,
    Normal,
    Full,
};

enum class FontSubpixelOrder {
    None,
    RGB,
    BGR,
    VerticalRGB,
    VerticalBGR,
};

struct FontRenderingOptions {
    FontHintingStyle hinting_style { FontHintingStyle::Normal };
    bool force_autohinting { false };
    bool allows_lcd_text { false };
};

// The FC_SLANT value for a slope of 0 for upright, 1 for italic or 2 for oblique.
int fontconfig_slant(u8 slope);

class GlobalFontConfig {
public:
    static GlobalFontConfig& the();
    FcConfig* get();

    FontRenderingOptions rendering_options_for_font(FlyString const& family, float pixel_size, u16 weight, u8 slope);
    FontSubpixelOrder subpixel_order() const;

private:
    friend class AK::NeverDestroyed<GlobalFontConfig>;

    GlobalFontConfig();
    ~GlobalFontConfig();

    GlobalFontConfig(GlobalFontConfig const&) = delete;
    GlobalFontConfig& operator=(GlobalFontConfig const&) = delete;

    FcConfig* m_config;
};

}
