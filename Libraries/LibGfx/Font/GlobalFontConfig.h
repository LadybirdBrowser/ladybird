/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Font.h>
#include <fontconfig/fontconfig.h>

namespace AK {

template<typename T>
class NeverDestroyed;

}

namespace Gfx {

class GlobalFontConfig {
public:
    static GlobalFontConfig& the();
    FcConfig* get();

    FontHintingOptions hinting_for_font(FlyString const& family, float pixel_size, u16 weight, u8 slope);

private:
    friend class AK::NeverDestroyed<GlobalFontConfig>;

    GlobalFontConfig();
    ~GlobalFontConfig();

    GlobalFontConfig(GlobalFontConfig const&) = delete;
    GlobalFontConfig& operator=(GlobalFontConfig const&) = delete;

    FcConfig* m_config;
};

}
