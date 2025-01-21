/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/TextLayout.h>

#include <core/SkFont.h>
#include <core/SkFontMetrics.h>
#include <core/SkFontTypes.h>

namespace Gfx {

ScaledFont::ScaledFont(NonnullRefPtr<Typeface> typeface, float point_width, float point_height, unsigned dpi_x, unsigned dpi_y)
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

ScaledFontMetrics ScaledFont::metrics() const
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

float ScaledFont::width(StringView view) const { return measure_text_width(Utf8View(view), *this, {}); }
float ScaledFont::width(Utf8View const& view) const { return measure_text_width(view, *this, {}); }

float ScaledFont::glyph_width(u32 code_point) const
{
    auto string = String::from_code_point(code_point);
    return measure_text_width(Utf8View(string), *this, {});
}

NonnullRefPtr<ScaledFont> ScaledFont::scaled_with_size(float point_size) const
{
    if (point_size == m_point_height && point_size == m_point_width)
        return *const_cast<ScaledFont*>(this);
    return m_typeface->scaled_font(point_size);
}

NonnullRefPtr<Font> ScaledFont::with_size(float point_size) const
{
    return scaled_with_size(point_size);
}

Gfx::FontPixelMetrics ScaledFont::pixel_metrics() const
{
    return m_pixel_metrics;
}

float ScaledFont::pixel_size() const
{
    return m_pixel_size;
}

int ScaledFont::pixel_size_rounded_up() const
{
    return m_pixel_size_rounded_up;
}

float ScaledFont::point_size() const
{
    return m_point_height;
}

}
