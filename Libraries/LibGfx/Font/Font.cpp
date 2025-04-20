/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/TextLayout.h>

#include <core/SkFont.h>
#include <core/SkFontMetrics.h>
#include <core/SkFontTypes.h>

#include <harfbuzz/hb.h>

namespace Gfx {

Font::Font(NonnullRefPtr<Typeface const> typeface, float point_width, float point_height, unsigned dpi_x, unsigned dpi_y)
    : m_typeface(move(typeface))
    , m_point_width(point_width)
    , m_point_height(point_height)
{
    float const units_per_em = m_typeface->units_per_em();
    m_x_scale = (point_width * dpi_x) / (POINTS_PER_INCH * units_per_em);
    m_y_scale = (point_height * dpi_y) / (POINTS_PER_INCH * units_per_em);

    m_pixel_size = m_point_height * (DEFAULT_DPI / POINTS_PER_INCH);
    m_pixel_size_rounded_up = static_cast<int>(ceilf(m_pixel_size));

    auto const* sk_typeface = as<TypefaceSkia>(*m_typeface).sk_typeface();
    SkFont const font { sk_ref_sp(sk_typeface), m_pixel_size };

    SkFontMetrics skMetrics;
    font.getMetrics(&skMetrics);

    FontPixelMetrics metrics;
    metrics.size = font.getSize();
    metrics.x_height = skMetrics.fXHeight;
    metrics.advance_of_ascii_zero = font.measureText("0", 1, SkTextEncoding::kUTF8);
    metrics.ascent = -skMetrics.fAscent;
    metrics.descent = skMetrics.fDescent;
    metrics.line_gap = skMetrics.fLeading;

    m_pixel_metrics = metrics;
}

ScaledFontMetrics Font::metrics() const
{
    SkFontMetrics sk_metrics;
    skia_font(1).getMetrics(&sk_metrics);

    ScaledFontMetrics metrics;
    metrics.ascender = -sk_metrics.fAscent;
    metrics.descender = sk_metrics.fDescent;
    metrics.line_gap = sk_metrics.fLeading;
    metrics.x_height = sk_metrics.fXHeight;
    return metrics;
}

float Font::width(StringView view) const { return measure_text_width(Utf8View(view), *this, {}); }
float Font::width(Utf8View const& view) const { return measure_text_width(view, *this, {}); }

float Font::glyph_width(u32 code_point) const
{
    auto string = String::from_code_point(code_point);
    return measure_text_width(Utf8View(string), *this, {});
}

NonnullRefPtr<Font> Font::scaled_with_size(float point_size) const
{
    if (point_size == m_point_height && point_size == m_point_width)
        return *const_cast<Font*>(this);
    return m_typeface->font(point_size);
}

NonnullRefPtr<Font> Font::with_size(float point_size) const
{
    return scaled_with_size(point_size);
}

Gfx::FontPixelMetrics Font::pixel_metrics() const
{
    return m_pixel_metrics;
}

float Font::pixel_size() const
{
    return m_pixel_size;
}

int Font::pixel_size_rounded_up() const
{
    return m_pixel_size_rounded_up;
}

float Font::point_size() const
{
    return m_point_height;
}

Font::~Font()
{
    if (m_harfbuzz_font)
        hb_font_destroy(m_harfbuzz_font);
}

Font const& Font::bold_variant() const
{
    if (m_bold_variant)
        return *m_bold_variant;
    m_bold_variant = Gfx::FontDatabase::the().get(family(), point_size(), 700, Gfx::FontWidth::Normal, 0);
    if (!m_bold_variant)
        m_bold_variant = this;
    return *m_bold_variant;
}

hb_font_t* Font::harfbuzz_font() const
{
    if (!m_harfbuzz_font) {
        m_harfbuzz_font = hb_font_create(typeface().harfbuzz_typeface());
        hb_font_set_scale(m_harfbuzz_font, pixel_size() * text_shaping_resolution, pixel_size() * text_shaping_resolution);
        hb_font_set_ptem(m_harfbuzz_font, point_size());
    }
    return m_harfbuzz_font;
}

}
