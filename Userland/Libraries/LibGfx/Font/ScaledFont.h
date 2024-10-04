/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>

class SkFont;

namespace Gfx {

class ScaledFont final : public Gfx::Font {
public:
    ScaledFont(NonnullRefPtr<Typeface>, float point_width, float point_height, unsigned dpi_x = DEFAULT_DPI, unsigned dpi_y = DEFAULT_DPI);
    ScaledFontMetrics metrics() const;

    // ^Gfx::Font
    virtual float point_size() const override;
    virtual float pixel_size() const override;
    virtual int pixel_size_rounded_up() const override;
    virtual Gfx::FontPixelMetrics pixel_metrics() const override;
    virtual u8 slope() const override { return m_typeface->slope(); }
    virtual u16 weight() const override { return m_typeface->weight(); }
    virtual bool contains_glyph(u32 code_point) const override { return m_typeface->glyph_id_for_code_point(code_point) > 0; }
    virtual float glyph_width(u32 code_point) const override;
    virtual u32 glyph_id_for_code_point(u32 code_point) const override { return m_typeface->glyph_id_for_code_point(code_point); }
    virtual float preferred_line_height() const override { return metrics().height() + metrics().line_gap; }
    virtual int x_height() const override { return m_point_height; } // FIXME: Read from font
    virtual u8 baseline() const override { return m_point_height; }  // FIXME: Read from font
    virtual float width(StringView) const override;
    virtual float width(Utf8View const&) const override;
    virtual String family() const override { return m_typeface->family(); }

    virtual NonnullRefPtr<ScaledFont> scaled_with_size(float point_size) const;
    virtual NonnullRefPtr<Font> with_size(float point_size) const override;

    virtual Typeface const& typeface() const override { return m_typeface; }

    SkFont skia_font(float scale) const;

private:
    NonnullRefPtr<Typeface> m_typeface;
    float m_x_scale { 0.0f };
    float m_y_scale { 0.0f };
    float m_point_width { 0.0f };
    float m_point_height { 0.0f };
    Gfx::FontPixelMetrics m_pixel_metrics;

    float m_pixel_size { 0.0f };
    int m_pixel_size_rounded_up { 0 };
};

}
