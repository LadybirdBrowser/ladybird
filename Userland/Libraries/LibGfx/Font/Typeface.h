/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontData.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Path.h>

#define POINTS_PER_INCH 72.0f
#define DEFAULT_DPI 96

class SkTypeface;

namespace Gfx {

class ScaledFont;

struct ScaledFontMetrics {
    float ascender { 0 };
    float descender { 0 };
    float line_gap { 0 };
    float x_height { 0 };

    float height() const
    {
        return ascender + descender;
    }
};

struct ScaledGlyphMetrics {
    float ascender;
    float descender;
    float advance_width;
    float left_side_bearing;
};

class Typeface : public RefCounted<Typeface> {
public:
    virtual ~Typeface();
    virtual ScaledFontMetrics metrics(float x_scale, float y_scale) const = 0;
    virtual ScaledGlyphMetrics glyph_metrics(u32 glyph_id, float x_scale, float y_scale, float point_width, float point_height) const = 0;
    virtual float glyph_advance(u32 glyph_id, float x_scale, float y_scale, float point_width, float point_height) const = 0;
    virtual float glyphs_horizontal_kerning(u32 left_glyph_id, u32 right_glyph_id, float x_scale) const = 0;
    virtual bool append_glyph_path_to(Gfx::Path&, u32 glyph_id, float x_scale, float y_scale) const = 0;

    virtual u32 glyph_count() const = 0;
    virtual u16 units_per_em() const = 0;
    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual String family() const = 0;
    virtual String variant() const = 0;
    virtual u16 weight() const = 0;
    virtual u16 width() const = 0;
    virtual u8 slope() const = 0;
    virtual bool is_fixed_width() const = 0;
    virtual bool has_color_bitmaps() const = 0;

    [[nodiscard]] NonnullRefPtr<ScaledFont> scaled_font(float point_size) const;

    RefPtr<SkTypeface> const& skia_typeface() const;

protected:
    Typeface();

    virtual ReadonlyBytes buffer() const = 0;
    virtual unsigned ttc_index() const = 0;

private:
    mutable HashMap<float, NonnullRefPtr<ScaledFont>> m_scaled_fonts;
    mutable RefPtr<SkTypeface> m_skia_typeface;
};

}
