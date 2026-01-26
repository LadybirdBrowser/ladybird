/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/FontData.h>
#include <LibGfx/Font/Typeface.h>

namespace Gfx {

class TypefaceSkia : public Gfx::Typeface {
    AK_MAKE_NONCOPYABLE(TypefaceSkia);

public:
    static ErrorOr<NonnullRefPtr<TypefaceSkia>> load_from_buffer(ReadonlyBytes, int index = 0);
    static ErrorOr<RefPtr<TypefaceSkia>> find_typeface_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope);
    static Optional<FlyString> resolve_generic_family(StringView family_name);

    RefPtr<TypefaceSkia const> clone_with_variations(Vector<FontVariationAxis> const& axes) const;

    virtual u32 glyph_count() const override;
    virtual u16 units_per_em() const override;
    virtual u32 glyph_id_for_code_point(u32 code_point) const override;
    virtual FlyString const& family() const override;
    virtual u16 weight() const override;
    virtual u16 width() const override;
    virtual u8 slope() const override;

    virtual ReadonlyBytes buffer() const LIFETIME_BOUND override { return m_buffer; }
    virtual unsigned ttc_index() const override { return m_ttc_index; }

    SkTypeface const* sk_typeface() const;

private:
    struct Impl;
    Impl& impl() const { return *m_impl; }
    NonnullOwnPtr<Impl> m_impl;

    explicit TypefaceSkia(NonnullOwnPtr<Impl>, ReadonlyBytes, int ttc_index = 0);

    virtual bool is_skia() const override { return true; }

    OwnPtr<FontData> m_font_data;
    ReadonlyBytes m_buffer;
    unsigned m_ttc_index { 0 };

    mutable Optional<FlyString> m_family;

    // This cache stores information per code point.
    // It's segmented into pages with data about 256 code points each.
    struct GlyphPage {
        static constexpr size_t glyphs_per_page = 256;
        u16 glyph_ids[glyphs_per_page];
    };

    // Fast cache for GlyphPage #0 (code points 0-255) to avoid hash lookups for all of ASCII and Latin-1.
    OwnPtr<GlyphPage> mutable m_glyph_page_zero;

    HashMap<size_t, NonnullOwnPtr<GlyphPage>> mutable m_glyph_pages;

    [[nodiscard]] GlyphPage const& glyph_page(size_t page_index) const;
    void populate_glyph_page(GlyphPage&, size_t page_index) const;
};

template<>
inline bool Typeface::fast_is<TypefaceSkia>() const { return is_skia(); }

}
