/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LsanSuppressions.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/TypefaceSkia.h>

#include <core/SkData.h>
#include <core/SkFontMgr.h>
#include <core/SkStream.h>
#include <core/SkTypeface.h>
#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_WINDOWS)
#    include <ports/SkTypeface_win.h>
#else
#    include <ports/SkFontMgr_fontconfig.h>
#    include <ports/SkFontScanner_FreeType.h>
#endif

#ifdef AK_OS_MACOS
#    include <ports/SkFontMgr_mac_ct.h>
#endif

namespace Gfx {

static sk_sp<SkFontMgr> s_font_manager;

struct TypefaceSkia::Impl {
    sk_sp<SkTypeface> skia_typeface;
};

static SkFontMgr& font_manager()
{
    if (!s_font_manager) {
#ifdef AK_OS_MACOS
        if (Gfx::FontDatabase::the().system_font_provider_name() != "FontConfig"sv) {
            s_font_manager = SkFontMgr_New_CoreText(nullptr);
        }
#endif
#if defined(AK_OS_ANDROID)
        s_font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_WINDOWS)
        s_font_manager = SkFontMgr_New_DirectWrite();
#else
        if (!s_font_manager) {
            s_font_manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
        }
#endif
    }
    VERIFY(s_font_manager);
    return *s_font_manager;
}

static SkFontStyle::Slant slope_to_skia_slant(u8 slope)
{
    switch (slope) {
    case 1:
        return SkFontStyle::kItalic_Slant;
    case 2:
        return SkFontStyle::kOblique_Slant;
    default:
        return SkFontStyle::kUpright_Slant;
    }
}

ErrorOr<NonnullRefPtr<TypefaceSkia>> TypefaceSkia::load_from_buffer(AK::ReadonlyBytes buffer, u32 ttc_index)
{
    auto data = SkData::MakeWithoutCopy(buffer.data(), buffer.size());
    auto skia_typeface = font_manager().makeFromData(data, static_cast<int>(ttc_index));

    if (!skia_typeface) {
        return Error::from_string_literal("Failed to load typeface from buffer");
    }

    return adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), buffer, ttc_index });
}

ErrorOr<RefPtr<TypefaceSkia>> TypefaceSkia::find_typeface_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope)
{
    SkFontStyle style(weight, width, slope_to_skia_slant(slope));

    auto skia_typeface = font_manager().matchFamilyStyleCharacter(
        nullptr, style, nullptr, 0, code_point);

    if (!skia_typeface)
        return RefPtr<TypefaceSkia> {};

    int skia_ttc_index = 0;
    auto stream = skia_typeface->openStream(&skia_ttc_index);
    auto ttc_index = static_cast<u32>(skia_ttc_index);

    if (stream && stream->getMemoryBase()) {
        auto buffer = TRY(ByteBuffer::copy({ static_cast<u8 const*>(stream->getMemoryBase()),
            stream->getLength() }));
        auto font_data = FontData::create_from_byte_buffer(move(buffer));
        auto bytes = font_data->bytes();

        auto result = adopt_ref(*new TypefaceSkia {
            make<TypefaceSkia::Impl>(skia_typeface),
            bytes,
            ttc_index });
        result->m_font_data = move(font_data);
        return result;
    }

    auto data = skia_typeface->serialize(SkTypeface::SerializeBehavior::kDoIncludeData);
    if (!data)
        return Error::from_string_literal("Failed to get font data from typeface");

    auto buffer = TRY(ByteBuffer::copy({ data->data(), data->size() }));
    auto font_data = FontData::create_from_byte_buffer(move(buffer));
    auto result = TRY(load_from_buffer(font_data->bytes(), ttc_index));
    result->m_font_data = move(font_data);
    return result;
}

Optional<FlyString> TypefaceSkia::resolve_generic_family(StringView family_name)
{
    SkFontStyle style(SkFontStyle::kNormal_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant);
    auto skia_typeface = font_manager().matchFamilyStyle(
        ByteString(family_name).characters(), style);

    if (!skia_typeface)
        return {};

    SkString resolved_family;
    skia_typeface->getFamilyName(&resolved_family);
    auto result_or_error = FlyString::from_utf8(StringView { resolved_family.c_str(), resolved_family.size() });
    if (result_or_error.is_error())
        return {};
    return result_or_error.release_value();
}

RefPtr<TypefaceSkia const> TypefaceSkia::clone_with_variations(Vector<FontVariationAxis> const& axes) const
{
    if (axes.is_empty())
        return this;

    SkFontArguments font_args;

    Vector<SkFontArguments::VariationPosition::Coordinate> coords;
    coords.ensure_capacity(axes.size());
    for (size_t i = 0; i < axes.size(); ++i) {
        coords.unchecked_append({ axes[i].tag.to_u32(), axes[i].value });
    }
    SkFontArguments::VariationPosition variation_pos;
    variation_pos.coordinates = coords.data();
    variation_pos.coordinateCount = static_cast<int>(coords.size());
    font_args.setVariationDesignPosition(variation_pos);

    font_args.setCollectionIndex(static_cast<int>(m_ttc_index));

    auto data = SkData::MakeWithoutCopy(m_buffer.data(), m_buffer.size());
    auto stream = std::make_unique<SkMemoryStream>(data);
    auto skia_typeface = font_manager().makeFromStream(std::move(stream), font_args);

    if (!skia_typeface)
        return {};

    return adopt_ref(*new TypefaceSkia { make<TypefaceSkia::Impl>(skia_typeface), m_buffer, m_ttc_index });
}

SkTypeface const* TypefaceSkia::sk_typeface() const
{
    return impl().skia_typeface.get();
}

TypefaceSkia::TypefaceSkia(NonnullOwnPtr<Impl> impl, ReadonlyBytes buffer, u32 ttc_index)
    : m_impl(move(impl))
    , m_buffer(buffer)
    , m_ttc_index(ttc_index)
{
}

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
    return glyph_page(code_point / GlyphPage::glyphs_per_page).glyph_ids[code_point % GlyphPage::glyphs_per_page];
}

TypefaceSkia::GlyphPage const& TypefaceSkia::glyph_page(size_t page_index) const
{
    if (page_index == 0) {
        if (!m_glyph_page_zero) {
            m_glyph_page_zero = make<GlyphPage>();
            populate_glyph_page(*m_glyph_page_zero, 0);
        }
        return *m_glyph_page_zero;
    }
    if (auto it = m_glyph_pages.find(page_index); it != m_glyph_pages.end()) {
        return *it->value;
    }

    auto glyph_page = make<GlyphPage>();
    populate_glyph_page(*glyph_page, page_index);
    auto const* glyph_page_ptr = glyph_page.ptr();
    m_glyph_pages.set(page_index, move(glyph_page));
    return *glyph_page_ptr;
}

void TypefaceSkia::populate_glyph_page(GlyphPage& glyph_page, size_t page_index) const
{
    u32 first_code_point = page_index * GlyphPage::glyphs_per_page;
    for (size_t i = 0; i < GlyphPage::glyphs_per_page; ++i) {
        u32 code_point = first_code_point + i;
        glyph_page.glyph_ids[i] = impl().skia_typeface->unicharToGlyph(code_point);
    }
}

FlyString const& TypefaceSkia::family() const
{
    return m_family.ensure([&] {
        SkString family_name;
        impl().skia_typeface->getFamilyName(&family_name);
        return FlyString::from_utf8_without_validation(ReadonlyBytes { family_name.c_str(), family_name.size() });
    });
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
