/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibGfx/Font/Emoji.h>
#include <LibGfx/Font/ScaledFont.h>

namespace Gfx {

ScaledFont::ScaledFont(NonnullRefPtr<Typeface> typeface, float point_width, float point_height, unsigned dpi_x, unsigned dpi_y)
    : m_typeface(move(typeface))
    , m_point_width(point_width)
    , m_point_height(point_height)
{
    float units_per_em = m_typeface->units_per_em();
    m_x_scale = (point_width * dpi_x) / (POINTS_PER_INCH * units_per_em);
    m_y_scale = (point_height * dpi_y) / (POINTS_PER_INCH * units_per_em);

    auto metrics = m_typeface->metrics(m_x_scale, m_y_scale);

    m_pixel_size = m_point_height * (DEFAULT_DPI / POINTS_PER_INCH);
    m_pixel_size_rounded_up = static_cast<int>(ceilf(m_pixel_size));

    m_pixel_metrics = Gfx::FontPixelMetrics {
        .size = (float)pixel_size(),
        .x_height = metrics.x_height,
        .advance_of_ascii_zero = (float)glyph_width('0'),
        .ascent = metrics.ascender,
        .descent = metrics.descender,
        .line_gap = metrics.line_gap,
    };
}

float ScaledFont::width(StringView view) const { return unicode_view_width(Utf8View(view)); }
float ScaledFont::width(Utf8View const& view) const { return unicode_view_width(view); }

template<typename T>
ALWAYS_INLINE float ScaledFont::unicode_view_width(T const& view) const
{
    if (view.is_empty())
        return 0;
    float width = 0;
    float longest_width = 0;
    u32 last_code_point = 0;

    for (auto it = view.begin(); it != view.end(); last_code_point = *it, ++it) {
        auto code_point = *it;

        if (code_point == '\n' || code_point == '\r') {
            longest_width = max(width, longest_width);
            width = 0;
            continue;
        }

        auto kerning = glyphs_horizontal_kerning(last_code_point, code_point);
        width += kerning + glyph_or_emoji_width(it);
    }

    longest_width = max(width, longest_width);
    return longest_width;
}

bool ScaledFont::append_glyph_path_to(Gfx::Path& path, u32 glyph_id) const
{
    return m_typeface->append_glyph_path_to(path, glyph_id, m_x_scale, m_y_scale);
}

float ScaledFont::glyph_left_bearing(u32 code_point) const
{
    auto id = glyph_id_for_code_point(code_point);
    return glyph_metrics(id).left_side_bearing;
}

float ScaledFont::glyph_width(u32 code_point) const
{
    auto id = glyph_id_for_code_point(code_point);
    return m_typeface->glyph_advance(id, m_x_scale, m_y_scale, m_point_width, m_point_height);
}

template<typename CodePointIterator>
static float glyph_or_emoji_width_impl(ScaledFont const& font, CodePointIterator& it)
{
    if (!font.has_color_bitmaps()) {
        if (auto const* emoji = Emoji::emoji_for_code_point_iterator(it))
            return font.pixel_size() * emoji->width() / emoji->height();
    }

    return font.glyph_width(*it);
}

float ScaledFont::glyph_or_emoji_width(Utf8CodePointIterator& it) const
{
    return glyph_or_emoji_width_impl(*this, it);
}

float ScaledFont::glyphs_horizontal_kerning(u32 left_code_point, u32 right_code_point) const
{
    if (left_code_point == 0 || right_code_point == 0)
        return 0.f;

    auto left_glyph_id = glyph_id_for_code_point(left_code_point);
    auto right_glyph_id = glyph_id_for_code_point(right_code_point);
    if (left_glyph_id == 0 || right_glyph_id == 0)
        return 0.f;

    return m_typeface->glyphs_horizontal_kerning(left_glyph_id, right_glyph_id, m_x_scale);
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
