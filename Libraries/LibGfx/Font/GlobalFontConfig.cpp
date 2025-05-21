/*
 * Copyright (c) 2025, blukai <init1@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
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

GlobalFontConfig::~GlobalFontConfig()
{
    FcConfigDestroy(m_config);
}

GlobalFontConfig& GlobalFontConfig::the()
{
    static GlobalFontConfig s_the;
    return s_the;
}

FcConfig* GlobalFontConfig::get()
{
    return m_config;
}

}
