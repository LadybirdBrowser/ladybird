/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LsanSuppressions.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>

#include <core/SkData.h>
#include <core/SkFontMgr.h>
#include <core/SkRefCnt.h>
#include <core/SkTypeface.h>
#ifndef AK_OS_ANDROID
#    include <ports/SkFontMgr_fontconfig.h>
#else
#    include <ports/SkFontMgr_android.h>
#endif

#ifdef AK_OS_MACOS
#    include <ports/SkFontMgr_mac_ct.h>
#endif

namespace Gfx {

static sk_sp<SkFontMgr> s_font_manager;

struct TypefaceSkia::Impl {
    sk_sp<SkTypeface> skia_typeface;
};

ErrorOr<NonnullRefPtr<TypefaceSkia>> TypefaceSkia::load_from_buffer(AK::ReadonlyBytes buffer, int ttc_index)
{
    if (!s_font_manager) {
#ifdef AK_OS_MACOS
        if (!Gfx::FontDatabase::the().should_force_fontconfig()) {
            s_font_manager = SkFontMgr_New_CoreText(nullptr);
        }
#endif
#ifndef AK_OS_ANDROID
        if (!s_font_manager) {
            s_font_manager = SkFontMgr_New_FontConfig(nullptr);
        }
#else
        s_font_manager = SkFontMgr_New_Android(nullptr);
#endif
    }

    auto data = SkData::MakeWithoutCopy(buffer.data(), buffer.size());
    auto skia_typeface = s_font_manager->makeFromData(data, ttc_index);

    if (!skia_typeface) {
        return Error::from_string_literal("Failed to load typeface from buffer");
    }

    return adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), buffer, ttc_index });
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
}

TypefaceSkia::TypefaceSkia(NonnullOwnPtr<Impl> impl, ReadonlyBytes buffer, int ttc_index)
    : m_impl(move(impl))
    , m_buffer(buffer)
    , m_ttc_index(ttc_index) {

    };

u32 TypefaceSkia::glyph_count() const
{
    return impl().skia_typeface->countGlyphs();
}

u16 TypefaceSkia::units_per_em() const
{
    return impl().skia_typeface->getUnitsPerEm();
}

u32 TypefaceSkia::glyph_id_for_code_point(u32 code_point) const
{
    return impl().skia_typeface->unicharToGlyph(code_point);
}

String TypefaceSkia::family() const
{
    SkString family_name;
    impl().skia_typeface->getFamilyName(&family_name);
    return String::from_utf8_without_validation(ReadonlyBytes { family_name.c_str(), family_name.size() });
}

u16 TypefaceSkia::weight() const
{
    return impl().skia_typeface->fontStyle().weight();
}

u16 TypefaceSkia::width() const
{
    return impl().skia_typeface->fontStyle().width();
}

u8 TypefaceSkia::slope() const
{
    auto slant = impl().skia_typeface->fontStyle().slant();
    switch (slant) {
    case SkFontStyle::kUpright_Slant:
        return 0;
    case SkFontStyle::kItalic_Slant:
        return 1;
    case SkFontStyle::kOblique_Slant:
        return 2;
    default:
        return 0;
    }
}

}
