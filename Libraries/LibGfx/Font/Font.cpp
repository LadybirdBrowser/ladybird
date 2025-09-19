/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf16String.h>
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

float Font::width(Utf16View const& view) const { return measure_text_width(view, *this, {}); }

float Font::glyph_width(u32 code_point) const
{
    auto string = Utf16String::from_code_point(code_point);
    return measure_text_width(string.utf16_view(), *this, {});
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

float Font::pixel_size() const
{
    return m_pixel_size;
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

SkFont Font::skia_font(float scale) const
{
    auto const& sk_typeface = as<TypefaceSkia>(*m_typeface).sk_typeface();
    auto sk_font = SkFont { sk_ref_sp(sk_typeface), pixel_size() * scale };
    sk_font.setSubpixel(true);
    return sk_font;
}

Font::ShapingCache::~ShapingCache()
{
    clear();
}

void Font::ShapingCache::clear()
{
    for (auto& it : map) {
        hb_buffer_destroy(it.value);
    }
    map.clear();
    for (auto& buffer : single_ascii_character_map) {
        if (buffer) {
            hb_buffer_destroy(buffer);
            buffer = nullptr;
        }
    }
}

}
