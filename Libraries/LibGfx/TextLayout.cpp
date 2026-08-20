/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/HashFunctions.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Point.h>
#include <LibGfx/TextLayout.h>
#include <LibUnicode/CharacterTypes.h>
#include <RustFFI.h>
#include <harfbuzz/hb.h>

namespace Gfx {

GlyphRun::GlyphRun(Vector<DrawGlyph>&& glyphs, NonnullRefPtr<Font const> font, TextType text_type, float width)
    : m_glyphs(move(glyphs))
    , m_font(move(font))
    , m_text_type(text_type)
    , m_width(width)
{
}

GlyphRun::~GlyphRun() = default;

NonnullRefPtr<GlyphRun> GlyphRun::slice(size_t start, size_t length) const
{
    Vector<DrawGlyph> sliced_glyphs;
    sliced_glyphs.ensure_capacity(length);

    float width = 0;
    for (size_t i = start; i < start + length; ++i) {
        sliced_glyphs.unchecked_append(m_glyphs[i]);
        width += m_glyphs[i].glyph_width;
    }

    return adopt_ref(*new GlyphRun(move(sliced_glyphs), m_font, m_text_type, width));
}

FloatRect GlyphRun::bounding_box(float scale) const
{
    auto font_ascent = m_font->pixel_metrics().ascent;

    // NOTE: This is a plain min/max rather than FloatRect::unite(), because a run with a single glyph must still
    //       produce a (zero-sized) origin box that gets expanded by the font bounds below.
    bool has_painted_glyphs = false;
    float min_x = NumericLimits<float>::max();
    float min_y = NumericLimits<float>::max();
    float max_x = NumericLimits<float>::lowest();
    float max_y = NumericLimits<float>::lowest();
    for (auto const& glyph : m_glyphs) {
        if (!glyph.should_paint)
            continue;
        auto origin_x = glyph.position.x() * scale;
        auto origin_y = (glyph.position.y() + font_ascent) * scale;
        if (!__builtin_isfinite(origin_x) || !__builtin_isfinite(origin_y))
            return {};
        min_x = min(min_x, origin_x);
        min_y = min(min_y, origin_y);
        max_x = max(max_x, origin_x);
        max_y = max(max_y, origin_y);
        has_painted_glyphs = true;
    }
    if (!has_painted_glyphs)
        return {};

    auto font_bounding_box = m_font->typeface().bounding_box_in_font_units();
    if (!font_bounding_box.is_empty()) {
        auto font_units_to_pixels = m_font->pixel_size() * scale / font_bounding_box.units_per_em;
        // Font units have y pointing up, device pixels have y pointing down.
        auto left = min_x + font_bounding_box.x_min * font_units_to_pixels;
        auto right = max_x + font_bounding_box.x_max * font_units_to_pixels;
        auto top = min_y - font_bounding_box.y_max * font_units_to_pixels;
        auto bottom = max_y - font_bounding_box.y_min * font_units_to_pixels;
        return { left, top, right - left, bottom - top };
    }

    // The font doesn't record an overall bounding box (e.g. bitmap-only fonts), so unite the glyphs' own extents.
    auto* hb_font = m_font->harfbuzz_font();
    int x_scale = 0;
    int y_scale = 0;
    hb_font_get_scale(hb_font, &x_scale, &y_scale);
    if (x_scale <= 0 || y_scale <= 0)
        return {};
    auto units_to_pixels_x = m_font->pixel_size() * scale / x_scale;
    auto units_to_pixels_y = m_font->pixel_size() * scale / y_scale;

    FloatRect bounds;
    for (auto const& glyph : m_glyphs) {
        if (!glyph.should_paint)
            continue;
        hb_glyph_extents_t extents;
        if (!hb_font_get_glyph_extents(hb_font, glyph.glyph_id, &extents))
            continue;
        auto origin_x = glyph.position.x() * scale;
        auto origin_y = (glyph.position.y() + font_ascent) * scale;
        // HarfBuzz extents have y pointing up, so `height` is negative for ink above the baseline.
        FloatRect glyph_bounds {
            origin_x + extents.x_bearing * units_to_pixels_x,
            origin_y - extents.y_bearing * units_to_pixels_y,
            extents.width * units_to_pixels_x,
            -extents.height * units_to_pixels_y,
        };
        bounds.unite(glyph_bounds);
    }
    return bounds;
}

namespace {

struct OutlinePoint {
    double x { 0 };
    double y { 0 };
};

// Roots of a*t^2 + b*t + c = 0 with t in [0, 1]. Duplicate roots may be reported twice.
size_t quadratic_roots_in_unit_interval(double a, double b, double c, Array<double, 2>& roots)
{
    size_t count = 0;
    auto add_root = [&](double t) {
        // NOTE: Written so that a NaN root (e.g. from a degenerate division) is rejected too.
        if (t >= 0 && t <= 1)
            roots[count++] = t;
    };

    if (a == 0) {
        if (b != 0)
            add_root(-c / b);
        return count;
    }

    auto discriminant = b * b - 4 * a * c;
    if (discriminant < 0)
        return 0;

    // The numerically stable form: computing the smaller root as c / q instead of (-b - sqrt(d)) / 2a avoids
    // catastrophic cancellation when |a| is tiny compared to |b| (a nearly-linear curve).
    auto q = -0.5 * (b + __builtin_copysign(AK::sqrt(discriminant), b));
    add_root(q / a);
    if (q != 0)
        add_root(c / q);
    return count;
}

// Roots of a*t^3 + b*t^2 + c*t + d = 0 with t in [0, 1]. The polynomial is split at the extrema of its derivative,
// so each piece is monotonic and has at most one root, which is then found by bisection. Unlike a closed-form
// solution, this can neither miss nor invent roots inside the interval.
size_t cubic_roots_in_unit_interval(double a, double b, double c, double d, Array<double, 3>& roots)
{
    auto evaluate = [&](double t) {
        return ((a * t + b) * t + c) * t + d;
    };

    Array<double, 4> breakpoints {};
    size_t breakpoint_count = 0;
    breakpoints[breakpoint_count++] = 0;
    Array<double, 2> extrema;
    auto extrema_count = quadratic_roots_in_unit_interval(3 * a, 2 * b, c, extrema);
    if (extrema_count == 2 && extrema[0] > extrema[1])
        swap(extrema[0], extrema[1]);
    for (size_t i = 0; i < extrema_count; ++i) {
        if (extrema[i] > 0 && extrema[i] < 1 && extrema[i] != breakpoints[breakpoint_count - 1])
            breakpoints[breakpoint_count++] = extrema[i];
    }
    breakpoints[breakpoint_count++] = 1;

    constexpr double value_tolerance = 1e-6;
    constexpr double parameter_tolerance = 1e-7;

    size_t count = 0;
    for (size_t i = 0; i + 1 < breakpoint_count; ++i) {
        auto start = breakpoints[i];
        auto end = breakpoints[i + 1];
        auto value_at_start = evaluate(start);
        auto value_at_end = evaluate(end);
        if (AK::fabs(value_at_start) <= value_tolerance) {
            roots[count++] = start;
            continue;
        }
        if (AK::fabs(value_at_end) <= value_tolerance) {
            roots[count++] = end;
            continue;
        }
        if ((value_at_start < 0) == (value_at_end < 0))
            continue;
        while (end - start > parameter_tolerance) {
            auto middle = 0.5 * (start + end);
            auto value_at_middle = evaluate(middle);
            if (value_at_middle == 0) {
                start = end = middle;
                break;
            }
            if ((value_at_middle < 0) == (value_at_start < 0)) {
                start = middle;
                value_at_start = value_at_middle;
            } else {
                end = middle;
            }
        }
        roots[count++] = 0.5 * (start + end);
    }
    return count;
}

// Tracks the horizontal extent of a glyph outline's ink inside a horizontal band, in glyph-local device pixels with
// y pointing down. This mirrors how Skia computes text blob intercepts: every point where an outline segment
// crosses the top or bottom edge of the band widens the extent, and so does every segment point (including
// control points) that lies strictly inside the band.
struct InkExtentInBand {
    double band_top { 0 };
    double band_bottom { 0 };
    double units_to_pixels_x { 0 };
    double units_to_pixels_y { 0 };
    double left { NumericLimits<double>::max() };
    double right { NumericLimits<double>::lowest() };

    OutlinePoint to_device_pixels(float x, float y) const
    {
        // HarfBuzz outlines have y pointing up.
        return { x * units_to_pixels_x, -y * units_to_pixels_y };
    }

    void expand(double x)
    {
        left = min(left, x);
        right = max(right, x);
    }

    bool overlaps_band(double segment_top, double segment_bottom) const
    {
        return band_top <= segment_bottom && segment_top <= band_bottom;
    }

    void expand_by_points_strictly_inside_band(ReadonlySpan<OutlinePoint> points)
    {
        for (auto const& point : points) {
            if (band_top < point.y && point.y < band_bottom)
                expand(point.x);
        }
    }

    void add_line(OutlinePoint p0, OutlinePoint p1)
    {
        if (!overlaps_band(min(p0.y, p1.y), max(p0.y, p1.y)))
            return;
        for (auto offset : { band_top, band_bottom }) {
            // NOTE: A horizontal line makes this a division by zero; the resulting inf/NaN fails the range check.
            auto t = (offset - p0.y) / (p1.y - p0.y);
            if (t >= 0 && t < 1)
                expand(p0.x + t * (p1.x - p0.x));
        }
        expand_by_points_strictly_inside_band(Array { p0, p1 });
    }

    void add_quadratic(OutlinePoint p0, OutlinePoint p1, OutlinePoint p2)
    {
        if (!overlaps_band(min(p0.y, min(p1.y, p2.y)), max(p0.y, max(p1.y, p2.y))))
            return;
        // y(t) = (y0 - 2*y1 + y2) * t^2 + 2 * (y1 - y0) * t + y0
        auto a = p0.y - 2 * p1.y + p2.y;
        auto b = 2 * (p1.y - p0.y);
        for (auto offset : { band_top, band_bottom }) {
            Array<double, 2> roots;
            auto count = quadratic_roots_in_unit_interval(a, b, p0.y - offset, roots);
            for (size_t i = 0; i < count; ++i) {
                auto t = roots[i];
                auto u = 1 - t;
                expand(u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x);
            }
        }
        expand_by_points_strictly_inside_band(Array { p0, p1, p2 });
    }

    void add_cubic(OutlinePoint p0, OutlinePoint p1, OutlinePoint p2, OutlinePoint p3)
    {
        if (!overlaps_band(min(min(p0.y, p1.y), min(p2.y, p3.y)), max(max(p0.y, p1.y), max(p2.y, p3.y))))
            return;
        // y(t) = (-y0 + 3*y1 - 3*y2 + y3) * t^3 + (3*y0 - 6*y1 + 3*y2) * t^2 + (-3*y0 + 3*y1) * t + y0
        auto a = -p0.y + 3 * p1.y - 3 * p2.y + p3.y;
        auto b = 3 * p0.y - 6 * p1.y + 3 * p2.y;
        auto c = -3 * p0.y + 3 * p1.y;
        for (auto offset : { band_top, band_bottom }) {
            Array<double, 3> roots;
            auto count = cubic_roots_in_unit_interval(a, b, c, p0.y - offset, roots);
            for (size_t i = 0; i < count; ++i) {
                auto t = roots[i];
                auto u = 1 - t;
                expand(u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x);
            }
        }
        expand_by_points_strictly_inside_band(Array { p0, p1, p2, p3 });
    }
};

// NOTE: HarfBuzz emits the implicit line that closes each contour as a regular line_to before close_path, so only
//       the segment callbacks are needed. The quadratic callback must be set, otherwise HarfBuzz converts quadratic
//       segments to cubics.
hb_draw_funcs_t* ink_extent_draw_funcs()
{
    static hb_draw_funcs_t* draw_funcs = [] {
        auto* funcs = hb_draw_funcs_create();
        hb_draw_funcs_set_line_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t* state, float to_x, float to_y, void*) {
                auto& ink_extent = *static_cast<InkExtentInBand*>(draw_data);
                ink_extent.add_line(
                    ink_extent.to_device_pixels(state->current_x, state->current_y),
                    ink_extent.to_device_pixels(to_x, to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t* state, float control_x, float control_y, float to_x, float to_y, void*) {
                auto& ink_extent = *static_cast<InkExtentInBand*>(draw_data);
                ink_extent.add_quadratic(
                    ink_extent.to_device_pixels(state->current_x, state->current_y),
                    ink_extent.to_device_pixels(control_x, control_y),
                    ink_extent.to_device_pixels(to_x, to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t* state, float control1_x, float control1_y, float control2_x, float control2_y, float to_x, float to_y, void*) {
                auto& ink_extent = *static_cast<InkExtentInBand*>(draw_data);
                ink_extent.add_cubic(
                    ink_extent.to_device_pixels(state->current_x, state->current_y),
                    ink_extent.to_device_pixels(control1_x, control1_y),
                    ink_extent.to_device_pixels(control2_x, control2_y),
                    ink_extent.to_device_pixels(to_x, to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_make_immutable(funcs);
        return funcs;
    }();
    return draw_funcs;
}

}

Vector<float> GlyphRun::get_glyph_intercepts(float scale, float y_top, float y_bottom) const
{
    if (!(scale > 0) || !__builtin_isfinite(scale))
        return {};

    auto* hb_font = m_font->harfbuzz_font();
    int x_scale = 0;
    int y_scale = 0;
    hb_font_get_scale(hb_font, &x_scale, &y_scale);
    if (x_scale <= 0 || y_scale <= 0)
        return {};
    auto units_to_pixels_x = static_cast<double>(m_font->pixel_size()) * scale / x_scale;
    auto units_to_pixels_y = static_cast<double>(m_font->pixel_size()) * scale / y_scale;
    auto font_ascent = m_font->pixel_metrics().ascent;

    Vector<float> intervals;
    for (auto const& glyph : m_glyphs) {
        if (!glyph.should_paint)
            continue;
        auto origin_x = static_cast<double>(glyph.position.x()) * scale;
        auto origin_y = (static_cast<double>(glyph.position.y()) + font_ascent) * scale;

        // Move the band into glyph-local coordinates, with the origin at the glyph's baseline origin.
        auto band_top = y_top - origin_y;
        auto band_bottom = y_bottom - origin_y;

        // Skip glyphs whose bounding box doesn't reach the band before decoding their outline.
        hb_glyph_extents_t extents;
        if (hb_font_get_glyph_extents(hb_font, glyph.glyph_id, &extents)) {
            auto glyph_top = -extents.y_bearing * units_to_pixels_y;
            auto glyph_bottom = -(extents.y_bearing + extents.height) * units_to_pixels_y;
            if (glyph_bottom < band_top || band_bottom < glyph_top)
                continue;
        }

        InkExtentInBand ink_extent {
            .band_top = band_top,
            .band_bottom = band_bottom,
            .units_to_pixels_x = units_to_pixels_x,
            .units_to_pixels_y = units_to_pixels_y,
        };
        hb_font_draw_glyph(hb_font, glyph.glyph_id, ink_extent_draw_funcs(), &ink_extent);

        // Glyphs without an outline (e.g. spaces) never widen the extent and produce no interval.
        if (ink_extent.left < ink_extent.right) {
            intervals.append(static_cast<float>(ink_extent.left + origin_x));
            intervals.append(static_cast<float>(ink_extent.right + origin_x));
        }
    }
    return intervals;
}

Vector<NonnullRefPtr<GlyphRun>> shape_text(FloatPoint baseline_start, Utf16View const& string, FontCascadeList const& font_cascade_list, float letter_spacing)
{
    if (string.is_empty())
        return {};

    Vector<NonnullRefPtr<GlyphRun>> runs;

    auto it = string.begin();
    auto substring_begin_offset = string.iterator_offset(it);
    Font const* last_font = &font_cascade_list.font_for_code_point(*it, FontCascadeList::TriggerPendingLoads::Yes);
    FloatPoint last_position = baseline_start;

    auto add_run = [&runs, &last_position, letter_spacing](Utf16View const& string, Font const& font) {
        auto run = shape_text(last_position, letter_spacing, 0.f, string, font, GlyphRun::TextType::Common);
        last_position.translate_by(run->width(), 0);
        runs.append(*run);
    };

    while (it != string.end()) {
        auto code_point = *it;
        auto const* font = &font_cascade_list.font_for_code_point(code_point, FontCascadeList::TriggerPendingLoads::Yes);
        if (font != last_font) {
            auto substring = string.substring_view(substring_begin_offset, string.iterator_offset(it) - substring_begin_offset);
            add_run(substring, *last_font);
            last_font = font;
            substring_begin_offset = string.iterator_offset(it);
        }
        ++it;
    }

    auto end_offset = string.iterator_offset(it);
    if (substring_begin_offset < end_offset) {
        auto substring = string.substring_view(substring_begin_offset, end_offset - substring_begin_offset);
        add_run(substring, *last_font);
    }

    return runs;
}

static hb_buffer_t* setup_text_shaping(Utf16View const& string, Font const& font, GlyphRun::TextType text_type)
{
    hb_buffer_t* buffer = hb_buffer_create();

    if (string.has_ascii_storage()) {
        hb_buffer_add_utf8(buffer, string.ascii_span().data(), string.length_in_code_units(), 0, -1);
        // Fast path for ASCII: we know it's Latin script, LTR direction.
        hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
        hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    } else {
        hb_buffer_add_utf16(buffer, reinterpret_cast<u16 const*>(string.utf16_span().data()), string.length_in_code_units(), 0, -1);
        // For non-ASCII, set direction from text_type if known, otherwise guess.
        if (text_type == GlyphRun::TextType::Ltr) {
            hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
            hb_buffer_guess_segment_properties(buffer);
        } else if (text_type == GlyphRun::TextType::Rtl) {
            hb_buffer_set_direction(buffer, HB_DIRECTION_RTL);
            hb_buffer_guess_segment_properties(buffer);
        } else {
            hb_buffer_guess_segment_properties(buffer);
        }
    }

    auto* hb_font = font.harfbuzz_font();
    hb_feature_t const* hb_features_data = nullptr;
    Vector<hb_feature_t, 4> hb_features;
    if (!font.features().is_empty()) {
        hb_features.ensure_capacity(font.features().size());
        for (auto const& feature : font.features()) {
            hb_features.unchecked_append({
                .tag = HB_TAG(feature.tag[0], feature.tag[1], feature.tag[2], feature.tag[3]),
                .value = feature.value,
                .start = 0,
                .end = HB_FEATURE_GLOBAL_END,
            });
        }
        hb_features_data = hb_features.data();
    }

    hb_shape(hb_font, buffer, hb_features_data, font.features().size());

    return buffer;
}

// https://drafts.csswg.org/css-text-4/#word-separator
static bool is_word_separator(u32 code_point)
{
    // Word-separator characters include the space (U+0020), the no-break space (U+00A0), the Ethiopic word space
    // (U+1361), the Aegean word separators (U+10100,U+10101), the Ugaritic word divider (U+1039F), and the Phoenician
    // Word Separator (U+1091F).
    // AD-HOC: Only the space and no-break space are treated as word separators, matching other engines. The line feed
    //         is also included because whitespace collapsing can leave a segment break in shaped text, where it
    //         behaves as a space.
    return code_point == 0x0020 || code_point == 0x00A0 || code_point == 0x000A;
}

static size_t length_of_trailing_whitespace_run(Utf16View const& string)
{
    size_t length = 0;
    while (length < string.length_in_code_units() && is_ascii_space(string.code_unit_at(string.length_in_code_units() - length - 1)))
        ++length;
    return length;
}

static NonnullOwnPtr<ShapedGlyphs> build_origin_relative_shape(Utf16View const& string, Font const& font, GlyphRun::TextType text_type, float letter_spacing, float word_spacing)
{
    auto const& metrics = font.pixel_metrics();
    auto* buffer = setup_text_shaping(string, font, text_type);

    u32 glyph_count;
    auto const* glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    auto const* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    Vector<DrawGlyph> glyphs;
    glyphs.ensure_capacity(glyph_count);
    FloatPoint point;

    // The trailing whitespace advance is recorded so that trimming it at line end can subtract exactly what shaping
    // added, spacing included.
    TrailingWhitespace trailing_whitespace { .length_in_code_units = length_of_trailing_whitespace_run(string), .advance = 0 };
    auto first_trailing_whitespace_offset = string.length_in_code_units() - trailing_whitespace.length_in_code_units;

    // We track the code unit length rather than just the code unit offset because LibWeb may later collapse glyph runs.
    // Updating the offset of each glyph gets tricky when handling text direction (LTR/RTL). So rather than doing that,
    // we just provide the glyph's code unit length and base LibWeb algorithms on that.
    //
    // A single grapheme may be represented by multiple glyphs, where any of those glyphs are zero-width. We want to
    // assign code unit lengths such that each glyph knows the length of the text it respresents.
    auto glyph_length_in_code_units = [&](auto index) -> size_t {
        auto starting_offset = glyph_info[index].cluster;

        for (size_t i = index + 1; i < glyph_count; ++i) {
            if (auto offset = glyph_info[i].cluster; offset != starting_offset)
                return offset - starting_offset;
        }

        return string.length_in_code_units() - starting_offset;
    };

    auto should_paint_glyph = [&](auto index) {
        auto starting_offset = glyph_info[index].cluster;
        if (starting_offset >= string.length_in_code_units())
            return true;
        return !Unicode::code_point_has_default_ignorable_code_point_property(string.code_point_at(starting_offset));
    };

    for (size_t i = 0; i < glyph_count; ++i) {
        bool should_paint = should_paint_glyph(i);
        auto position = point
            - FloatPoint { 0, metrics.ascent }
            + FloatPoint { positions[i].x_offset, positions[i].y_offset } / text_shaping_resolution;

        auto extra_advance = letter_spacing;
        if (auto starting_offset = glyph_info[i].cluster; starting_offset < string.length_in_code_units()
            && is_word_separator(string.code_point_at(starting_offset)))
            extra_advance += word_spacing;

        glyphs.unchecked_append({
            .position = position,
            .length_in_code_units = glyph_length_in_code_units(i),
            .glyph_width = should_paint ? positions[i].x_advance / text_shaping_resolution + extra_advance : 0,
            .glyph_id = glyph_info[i].codepoint,
            .should_paint = should_paint,
        });

        if (!should_paint)
            continue;

        point += FloatPoint { positions[i].x_advance, positions[i].y_advance } / text_shaping_resolution;

        // NOTE: The spec says that we "really should not" apply letter-spacing to the trailing edge of a line but
        //       other browsers do so we will as well. https://drafts.csswg.org/css-text/#example-7880704e
        point.translate_by(extra_advance, 0);

        if (glyph_info[i].cluster >= first_trailing_whitespace_offset)
            trailing_whitespace.advance += glyphs.last().glyph_width;
    }

    hb_buffer_destroy(buffer);

    return make<ShapedGlyphs>(move(glyphs), point.x(), trailing_whitespace);
}

NonnullRefPtr<GlyphRun> shape_text(FloatPoint baseline_start, float letter_spacing, float word_spacing, Utf16View const& string, Font const& font, GlyphRun::TextType text_type, TrailingWhitespace* out_trailing_whitespace)
{
    auto& shaping_cache = font.shaping_cache();

    auto build_glyph_run = [&](ShapedGlyphs const& shape) -> NonnullRefPtr<GlyphRun> {
        if (out_trailing_whitespace)
            *out_trailing_whitespace = shape.trailing_whitespace;
        Vector<DrawGlyph> glyphs = shape.glyphs;
        if (!baseline_start.is_zero()) {
            for (auto& glyph : glyphs)
                glyph.position.translate_by(baseline_start);
        }
        return adopt_ref(*new GlyphRun(move(glyphs), font, text_type, shape.width));
    };

    // FIXME: The cache currently grows unbounded. We should have some limit and LRU mechanism.
    if (string.length_in_code_units() == 1 && letter_spacing == 0.f && word_spacing == 0.f && text_type == GlyphRun::TextType::Common) {
        auto code_unit = string.code_unit_at(0);
        if (code_unit < 128) {
            auto& cache_slot = shaping_cache.single_ascii_character_map[code_unit];
            if (!cache_slot)
                cache_slot = build_origin_relative_shape(string, font, text_type, letter_spacing, word_spacing);
            return build_glyph_run(*cache_slot);
        }
    }

    auto text_type_bits = static_cast<u8>(to_underlying(text_type));
    auto letter_spacing_bit_pattern = bit_cast<u32>(letter_spacing);
    auto word_spacing_bit_pattern = bit_cast<u32>(word_spacing);
    auto key_hash = pair_int_hash(string.hash(), pair_int_hash(text_type_bits, pair_int_hash(letter_spacing_bit_pattern, word_spacing_bit_pattern)));

    if (auto it = shaping_cache.map.find(key_hash, [&](auto const& candidate) {
            return candidate.key.text_type == text_type_bits
                && candidate.key.letter_spacing_bit_pattern == letter_spacing_bit_pattern
                && candidate.key.word_spacing_bit_pattern == word_spacing_bit_pattern
                && candidate.key.text == string;
        });
        it != shaping_cache.map.end()) {
        return build_glyph_run(*it->value);
    }

    auto shape = build_origin_relative_shape(string, font, text_type, letter_spacing, word_spacing);
    auto run = build_glyph_run(*shape);
    shaping_cache.map.set({ Utf16String::from_utf16(string), text_type_bits, letter_spacing_bit_pattern, word_spacing_bit_pattern }, move(shape));
    return run;
}

float measure_text_width(Utf16View const& string, Font const& font, float letter_spacing)
{
    auto* buffer = setup_text_shaping(string, font, GlyphRun::TextType::Common);

    u32 glyph_count;
    auto const* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    double point_x = 0;
    for (size_t i = 0; i < glyph_count; ++i)
        point_x += positions[i].x_advance;

    hb_buffer_destroy(buffer);
    return static_cast<float>(point_x / text_shaping_resolution + glyph_count * letter_spacing);
}

}

static_assert(to_underlying(Gfx::FFI::TextType::Common) == to_underlying(Gfx::GlyphRun::TextType::Common));
static_assert(to_underlying(Gfx::FFI::TextType::ContextDependent) == to_underlying(Gfx::GlyphRun::TextType::ContextDependent));
static_assert(to_underlying(Gfx::FFI::TextType::EndPadding) == to_underlying(Gfx::GlyphRun::TextType::EndPadding));
static_assert(to_underlying(Gfx::FFI::TextType::Ltr) == to_underlying(Gfx::GlyphRun::TextType::Ltr));
static_assert(to_underlying(Gfx::FFI::TextType::Rtl) == to_underlying(Gfx::GlyphRun::TextType::Rtl));
static_assert(sizeof(Gfx::FFI::DrawGlyph) == sizeof(Gfx::DrawGlyph));
static_assert(alignof(Gfx::FFI::DrawGlyph) == alignof(Gfx::DrawGlyph));
static_assert(IsTriviallyCopyable<Gfx::FFI::DrawGlyph>);
static_assert(IsTriviallyCopyable<Gfx::DrawGlyph>);
static_assert(sizeof(Gfx::FloatPoint) == 2 * sizeof(float));
static_assert(offsetof(Gfx::FFI::DrawGlyph, x) == offsetof(Gfx::DrawGlyph, position));
static_assert(offsetof(Gfx::FFI::DrawGlyph, y) == offsetof(Gfx::DrawGlyph, position) + sizeof(float));
static_assert(offsetof(Gfx::FFI::DrawGlyph, length_in_code_units) == offsetof(Gfx::DrawGlyph, length_in_code_units));
static_assert(offsetof(Gfx::FFI::DrawGlyph, glyph_width) == offsetof(Gfx::DrawGlyph, glyph_width));
static_assert(offsetof(Gfx::FFI::DrawGlyph, glyph_id) == offsetof(Gfx::DrawGlyph, glyph_id));
static_assert(offsetof(Gfx::FFI::DrawGlyph, should_paint) == offsetof(Gfx::DrawGlyph, should_paint));

extern "C" {
Gfx::FFI::ShapedRunView ladybird_gfx_shape_text(void const*, u16 const*, size_t, Gfx::FFI::TextType, float, float, float);
void ladybird_gfx_glyph_run_unref(void*);
void* ladybird_gfx_glyph_run_create(void const*, Gfx::FFI::DrawGlyph const*, size_t, Gfx::FFI::TextType, float);
void ladybird_gfx_glyph_run_bounding_box(void const*, float, float*);
void ladybird_gfx_glyph_run_glyph_intercepts(void const*, float, float, float, void*, void (*)(void*, float));
}

extern "C" Gfx::FFI::ShapedRunView ladybird_gfx_shape_text(
    void const* font,
    u16 const* text_utf16,
    size_t length_in_code_units,
    Gfx::FFI::TextType text_type,
    float baseline_start_x,
    float letter_spacing,
    float word_spacing)
{
    VERIFY(font);
    VERIFY(text_utf16 || length_in_code_units == 0);
    auto text = length_in_code_units == 0
        ? Utf16View {}
        : Utf16View { reinterpret_cast<char16_t const*>(text_utf16), length_in_code_units };
    Gfx::TrailingWhitespace trailing_whitespace;
    auto run = Gfx::shape_text(
        { baseline_start_x, 0 },
        letter_spacing,
        word_spacing,
        text,
        *static_cast<Gfx::Font const*>(font),
        static_cast<Gfx::GlyphRun::TextType>(text_type),
        &trailing_whitespace);
    auto* retained = &run.leak_ref();
    return {
        .glyphs = reinterpret_cast<Gfx::FFI::DrawGlyph const*>(retained->glyphs().data()),
        .glyph_count = retained->glyphs().size(),
        .width = retained->width(),
        .trailing_whitespace_length_in_code_units = trailing_whitespace.length_in_code_units,
        .trailing_whitespace_advance = trailing_whitespace.advance,
        .retained = retained,
    };
}

extern "C" void ladybird_gfx_glyph_run_unref(void* retained)
{
    VERIFY(retained);
    static_cast<Gfx::GlyphRun*>(retained)->unref();
}

extern "C" void* ladybird_gfx_glyph_run_create(
    void const* font,
    Gfx::FFI::DrawGlyph const* glyphs,
    size_t glyph_count,
    Gfx::FFI::TextType text_type,
    float width)
{
    VERIFY(font);
    VERIFY(glyphs || glyph_count == 0);
    Vector<Gfx::DrawGlyph> copied_glyphs;
    copied_glyphs.ensure_capacity(glyph_count);
    copied_glyphs.unchecked_append(reinterpret_cast<Gfx::DrawGlyph const*>(glyphs), glyph_count);
    auto run = adopt_ref(*new Gfx::GlyphRun(
        move(copied_glyphs),
        *static_cast<Gfx::Font const*>(font),
        static_cast<Gfx::GlyphRun::TextType>(text_type),
        width));
    return &run.leak_ref();
}

extern "C" void ladybird_gfx_glyph_run_bounding_box(void const* retained, float scale, float* out_rect)
{
    VERIFY(retained);
    VERIFY(out_rect);
    auto bounds = static_cast<Gfx::GlyphRun const*>(retained)->bounding_box(scale);
    out_rect[0] = bounds.x();
    out_rect[1] = bounds.y();
    out_rect[2] = bounds.width();
    out_rect[3] = bounds.height();
}

extern "C" void ladybird_gfx_glyph_run_glyph_intercepts(
    void const* retained,
    float scale,
    float y_top,
    float y_bottom,
    void* sink,
    void (*push)(void*, float))
{
    VERIFY(retained);
    VERIFY(push);
    for (auto value : static_cast<Gfx::GlyphRun const*>(retained)->get_glyph_intercepts(scale, y_top, y_bottom))
        push(sink, value);
}
