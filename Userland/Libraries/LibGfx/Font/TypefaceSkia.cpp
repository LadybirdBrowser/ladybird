/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <LibGfx/Font/Typeface.h>

#include <core/SkData.h>
#include <core/SkFontMgr.h>
#include <core/SkTypeface.h>

#ifdef AK_OS_MACOS
#    include <ports/SkFontMgr_mac_ct.h>
#else
#    include <ports/SkFontMgr_fontconfig.h>
#endif

namespace Gfx {

static sk_sp<SkFontMgr> s_font_manager;

RefPtr<SkTypeface> const& Typeface::skia_typeface() const
{
    if (!s_font_manager) {
#ifdef AK_OS_MACOS
        s_font_manager = SkFontMgr_New_CoreText(nullptr);
#else
        s_font_manager = SkFontMgr_New_FontConfig(nullptr);
#endif
    }

    if (!m_skia_typeface) {
        auto data = SkData::MakeWithoutCopy(buffer().data(), buffer().size());
        auto skia_typeface = s_font_manager->makeFromData(data, ttc_index());
        if (!skia_typeface)
            VERIFY_NOT_REACHED();
        m_skia_typeface = *skia_typeface;
    }
    return m_skia_typeface;
}

}
