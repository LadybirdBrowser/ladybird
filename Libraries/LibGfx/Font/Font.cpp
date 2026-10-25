/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16String.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontTable.h>
#include <LibGfx/TextLayout.h>
#include <RustFFI.h>

#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

extern "C" {
void ladybird_gfx_font_snapshot(void const*, Gfx::FFI::FfiFontSnapshot*);
u32 ladybird_gfx_font_glyph_id(void const*, u32);
bool ladybird_gfx_font_contains_glyph(void const*, u32);
bool ladybird_gfx_font_is_emoji_font(void const*);
bool ladybird_gfx_font_is_invisible(void const*);
Gfx::FFI::FfiMathData const* ladybird_gfx_font_math_data(void const*);
void ladybird_gfx_font_glyph_metrics(void const*, u32, Gfx::FFI::FfiGlyphMetrics*);
size_t ladybird_gfx_font_math_glyph_variants(void const*, u32, bool, Gfx::FFI::FfiMathGlyphVariant*, size_t);
size_t ladybird_gfx_font_math_glyph_assembly(void const*, u32, bool, Gfx::FFI::FfiMathGlyphPart*, size_t, float*);
void ladybird_gfx_font_ref(void const*);
void ladybird_gfx_font_unref(void const*);
}

namespace Gfx {

static Atomic<u64> s_next_id { 1 };

struct Font::MathData {
    AK_ALLOC_WITH_KMALLOC;

public:
    FFI::FfiMathData data {};
};

Font::Font(NonnullRefPtr<Typeface const> typeface, float point_width, float point_height, FontVariationSettings const variations, ShapeFeatures const& features)
    : m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_typeface(move(typeface))
    , m_point_width(point_width)
    , m_point_height(point_height)
    , m_font_variation_settings(move(variations))
    , m_shape_features(features)
{
    m_pixel_size = m_point_height * (DEFAULT_DPI / POINTS_PER_INCH);
    m_style = m_typeface->style_for_variations(m_font_variation_settings.to_sorted_list());
    m_harfbuzz_font = create_harfbuzz_font();
    m_pixel_metrics = compute_pixel_metrics();
}

FontPixelMetrics Font::compute_pixel_metrics() const
{
    auto const& vertical_metrics = m_typeface->description().vertical_metrics();
    auto units_per_em = static_cast<float>(m_typeface->units_per_em());
    auto font_units_to_pixels = [&](float units) {
        return units / units_per_em * m_pixel_size;
    };

    FontPixelMetrics metrics;
    metrics.ascent = font_units_to_pixels(vertical_metrics.ascender + hb_ot_metrics_get_variation(m_harfbuzz_font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER));
    metrics.descent = -font_units_to_pixels(vertical_metrics.descender + hb_ot_metrics_get_variation(m_harfbuzz_font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER));

    // https://drafts.csswg.org/css-values-4/#ex
    // In the cases where it is impossible or impractical to determine the x-height, a value of 0.5em must be assumed.
    metrics.x_height = m_pixel_size / 2;
    int x_scale = 0;
    int y_scale = 0;
    hb_font_get_scale(m_harfbuzz_font, &x_scale, &y_scale);
    hb_codepoint_t glyph_id = 0;
    hb_glyph_extents_t extents {};
    if (vertical_metrics.x_height != 0) {
        metrics.x_height = font_units_to_pixels(vertical_metrics.x_height + hb_ot_metrics_get_variation(m_harfbuzz_font, HB_OT_METRICS_TAG_X_HEIGHT));
    } else if (y_scale > 0 && hb_font_get_nominal_glyph(m_harfbuzz_font, 'x', &glyph_id) && hb_font_get_glyph_extents(m_harfbuzz_font, glyph_id, &extents) && extents.y_bearing > 0) {
        metrics.x_height = static_cast<float>(extents.y_bearing) * m_pixel_size / static_cast<float>(y_scale);
    }

    // https://drafts.csswg.org/css-values-4/#ch
    // The advance of the glyph that shaping uses for "0". In the cases where it is impossible or impractical to
    // determine the measure of the "0" glyph, it must be assumed to be 0.5em wide.
    metrics.advance_of_ascii_zero = m_pixel_size / 2;
    if (hb_font_get_nominal_glyph(m_harfbuzz_font, '0', &glyph_id))
        metrics.advance_of_ascii_zero = static_cast<float>(hb_font_get_glyph_h_advance(m_harfbuzz_font, glyph_id)) / text_shaping_resolution;

    return metrics;
}

float Font::cap_height() const
{
    // https://drafts.csswg.org/css-values-4/#cap
    // In the cases where it is impossible or impractical to determine the cap-height, the font’s ascent must be used.
    hb_position_t cap_height = 0;
    if (hb_ot_metrics_get_position(m_harfbuzz_font, HB_OT_METRICS_TAG_CAP_HEIGHT, &cap_height) && cap_height > 0)
        return static_cast<float>(cap_height) / text_shaping_resolution;
    return m_pixel_metrics.ascent;
}

Optional<float> Font::horizontal_advance_of_code_point(u32 code_point) const
{
    hb_codepoint_t glyph_id = 0;
    if (!hb_font_get_nominal_glyph(m_harfbuzz_font, code_point, &glyph_id))
        return {};
    return static_cast<float>(hb_font_get_glyph_h_advance(m_harfbuzz_font, glyph_id)) / text_shaping_resolution;
}

Optional<float> Font::vertical_advance_of_code_point(u32 code_point) const
{
    hb_codepoint_t glyph_id = 0;
    if (!hb_font_get_nominal_glyph(m_harfbuzz_font, code_point, &glyph_id))
        return {};
    // NB: HarfBuzz measures vertical advances downwards as negative.
    auto advance = hb_font_get_glyph_v_advance(m_harfbuzz_font, glyph_id);
    return static_cast<float>(advance < 0 ? -advance : advance) / text_shaping_resolution;
}

float Font::width(Utf16View const& view) const { return measure_text_width(view, *this); }

NonnullRefPtr<Font> Font::invisible_variant() const
{
    return m_typeface->invisible_font(m_point_height, m_font_variation_settings, m_shape_features);
}

float Font::pixel_size() const
{
    return m_pixel_size;
}

float Font::point_size() const
{
    return m_point_height;
}

void Font::will_be_destroyed() const
{
    m_typeface->forget_font(*this);
}

Font::~Font()
{
    if (m_harfbuzz_font)
        hb_font_destroy(m_harfbuzz_font);
}

static int scale_for_harfbuzz(float pixel_size)
{
    auto scaled_pixel_size = static_cast<double>(pixel_size) * text_shaping_resolution;
    if (__builtin_isnan(scaled_pixel_size))
        return 0;
    if (scaled_pixel_size >= NumericLimits<int>::max())
        return NumericLimits<int>::max();
    if (scaled_pixel_size <= NumericLimits<int>::min())
        return NumericLimits<int>::min();
    return static_cast<int>(scaled_pixel_size);
}

hb_font_t* Font::create_harfbuzz_font() const
{
    auto* font = hb_font_create(typeface().harfbuzz_typeface());
    auto harfbuzz_scale = scale_for_harfbuzz(pixel_size());
    hb_font_set_scale(font, harfbuzz_scale, harfbuzz_scale);
    // HarfBuzz uses ptem for AAT 'trak' table lookup; use CSS pixels instead of physical points here.
    hb_font_set_ptem(font, pixel_size());

    auto variations = m_font_variation_settings.axes;
    if (!variations.is_empty()) {
        Vector<hb_variation_t> hb_list;
        hb_list.ensure_capacity(variations.size());

        for (auto const& axis : variations) {
            hb_list.unchecked_append(hb_variation_t { axis.key.to_u32(), axis.value });
        }

        hb_font_set_variations(font, hb_list.data(), hb_list.size());
    }
    hb_font_make_immutable(font);
    return font;
}

bool Font::is_emoji_font() const
{
    if (m_is_emoji_font.load(AK::MemoryOrder::memory_order_relaxed) == TriState::Unknown) {
        // NOTE: This is a heuristic approach to determine if a font is an emoji font.
        //       AFAIK there is no definitive way to know this from the font data itself.

        // 1. If the family name contains "emoji", it's probably an emoji font.
        bool name_contains_emoji = family().bytes_as_string_view().contains("emoji"sv);

        // 2. Check for color font tables and absence of regular text glyphs.
        auto* hb_font = harfbuzz_font();
        hb_face_t* face = hb_font_get_face(hb_font);

        // hb_ot_color_has_layers() only reports COLRv0 layered glyphs; COLRv1 fonts (e.g. Noto Color Emoji's COLRv1
        // build) carry a paint graph instead, reported by hb_ot_color_has_paint().
        bool has_colr = hb_ot_color_has_layers(face) || hb_ot_color_has_paint(face);
        bool has_svg = hb_ot_color_has_svg(face);

        bool has_sbix = face_has_table(face, FourCC { "sbix" });
        bool has_cbdt = face_has_table(face, FourCC { "CBDT" });
        bool has_cblc = face_has_table(face, FourCC { "CBLC" });
        bool has_any_color = has_colr || has_svg || has_sbix || (has_cbdt && has_cblc);

        auto looks_like_text = [&]() {
            hb_codepoint_t uppercase_a_glyph_id = 0;
            hb_codepoint_t lowercase_a_glyph_id = 0;
            bool has_uppercase_a = hb_font_get_nominal_glyph(hb_font, 'A', &uppercase_a_glyph_id);
            bool has_lowercase_a = hb_font_get_nominal_glyph(hb_font, 'a', &lowercase_a_glyph_id);
            return has_uppercase_a && has_lowercase_a;
        }();

        auto verdict = (name_contains_emoji && !looks_like_text) || (has_any_color && !looks_like_text) ? TriState::True : TriState::False;
        m_is_emoji_font.store(verdict, AK::MemoryOrder::memory_order_relaxed);
        return verdict == TriState::True;
    }

    return m_is_emoji_font.load(AK::MemoryOrder::memory_order_relaxed) == TriState::True;
}

static float harfbuzz_position_to_pixels(hb_position_t position)
{
    return static_cast<float>(position) / text_shaping_resolution;
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/math#mathconstants-table
Font::MathData const& Font::math_data() const
{
    call_once(m_math_data_once, [&] {
        auto math_data = make<MathData>();
        auto* data = &math_data->data;
        data->has_math_table = hb_ot_math_has_data(hb_font_get_face(m_harfbuzz_font));
        if (data->has_math_table) {
            static_assert(HB_OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT + 1 == sizeof(FFI::FfiMathData::constants) / sizeof(float));
            for (size_t index = 0; index <= HB_OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT; ++index) {
                auto constant = static_cast<hb_ot_math_constant_t>(index);
                auto value = hb_ot_math_get_constant(m_harfbuzz_font, constant);
                // NB: HarfBuzz scales every constant to the font size, except for the three percentages.
                bool is_percentage = constant == HB_OT_MATH_CONSTANT_SCRIPT_PERCENT_SCALE_DOWN
                    || constant == HB_OT_MATH_CONSTANT_SCRIPT_SCRIPT_PERCENT_SCALE_DOWN
                    || constant == HB_OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT;
                data->constants[index] = is_percentage ? static_cast<float>(value) : harfbuzz_position_to_pixels(value);
            }
            data->horizontal_min_connector_overlap = harfbuzz_position_to_pixels(hb_ot_math_get_min_connector_overlap(m_harfbuzz_font, HB_DIRECTION_LTR));
            data->vertical_min_connector_overlap = harfbuzz_position_to_pixels(hb_ot_math_get_min_connector_overlap(m_harfbuzz_font, HB_DIRECTION_BTT));
        }

        hb_position_t position = 0;
        data->has_underline_thickness = hb_ot_metrics_get_position(m_harfbuzz_font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &position);
        data->underline_thickness = harfbuzz_position_to_pixels(position);

        m_math_data = move(math_data);
    });
    return *m_math_data;
}

}

extern "C" void ladybird_gfx_font_snapshot(void const* font, Gfx::FFI::FfiFontSnapshot* out_snapshot)
{
    VERIFY(font);
    VERIFY(out_snapshot);
    auto const& typed_font = *static_cast<Gfx::Font const*>(font);
    auto const& metrics = typed_font.pixel_metrics();
    *out_snapshot = {
        .id = typed_font.id(),
        .ascent = metrics.ascent,
        .descent = metrics.descent,
        .x_height = metrics.x_height,
        .zero_advance = metrics.advance_of_ascii_zero,
        .pixel_size = typed_font.pixel_size(),
        .point_size = typed_font.point_size(),
    };
}

extern "C" u32 ladybird_gfx_font_glyph_id(void const* font, u32 code_point)
{
    VERIFY(font);
    return static_cast<Gfx::Font const*>(font)->glyph_id_for_code_point(code_point);
}

extern "C" bool ladybird_gfx_font_contains_glyph(void const* font, u32 code_point)
{
    VERIFY(font);
    return static_cast<Gfx::Font const*>(font)->contains_glyph(code_point);
}

extern "C" bool ladybird_gfx_font_is_emoji_font(void const* font)
{
    VERIFY(font);
    return static_cast<Gfx::Font const*>(font)->is_emoji_font();
}

extern "C" bool ladybird_gfx_font_is_invisible(void const* font)
{
    VERIFY(font);
    return static_cast<Gfx::Font const*>(font)->is_invisible();
}

extern "C" Gfx::FFI::FfiMathData const* ladybird_gfx_font_math_data(void const* font)
{
    VERIFY(font);
    return &static_cast<Gfx::Font const*>(font)->math_data().data;
}

extern "C" void ladybird_gfx_font_glyph_metrics(void const* font, u32 glyph_id, Gfx::FFI::FfiGlyphMetrics* out_metrics)
{
    VERIFY(font);
    VERIFY(out_metrics);
    auto* harfbuzz_font = static_cast<Gfx::Font const*>(font)->harfbuzz_font();
    hb_glyph_extents_t extents {};
    if (!hb_font_get_glyph_extents(harfbuzz_font, glyph_id, &extents))
        extents = {};
    *out_metrics = {
        .advance = Gfx::harfbuzz_position_to_pixels(hb_font_get_glyph_h_advance(harfbuzz_font, glyph_id)),
        .ascent = Gfx::harfbuzz_position_to_pixels(extents.y_bearing),
        .descent = -Gfx::harfbuzz_position_to_pixels(extents.y_bearing + extents.height),
    };
}

static hb_direction_t math_stretch_direction(bool horizontal)
{
    return horizontal ? HB_DIRECTION_LTR : HB_DIRECTION_BTT;
}

extern "C" size_t ladybird_gfx_font_math_glyph_variants(void const* font, u32 glyph_id, bool horizontal, Gfx::FFI::FfiMathGlyphVariant* out_variants, size_t capacity)
{
    VERIFY(font);
    VERIFY(out_variants || capacity == 0);
    auto* harfbuzz_font = static_cast<Gfx::Font const*>(font)->harfbuzz_font();
    Vector<hb_ot_math_glyph_variant_t, 8> variants;
    variants.resize(capacity);
    unsigned count = capacity;
    auto total = hb_ot_math_get_glyph_variants(harfbuzz_font, glyph_id, math_stretch_direction(horizontal), 0, &count, variants.data());
    for (size_t index = 0; index < count; ++index) {
        out_variants[index] = {
            .glyph_id = variants[index].glyph,
            .advance_measurement = Gfx::harfbuzz_position_to_pixels(variants[index].advance),
        };
    }
    return total;
}

extern "C" size_t ladybird_gfx_font_math_glyph_assembly(void const* font, u32 glyph_id, bool horizontal, Gfx::FFI::FfiMathGlyphPart* out_parts, size_t capacity, float* out_italics_correction)
{
    VERIFY(font);
    VERIFY(out_parts || capacity == 0);
    VERIFY(out_italics_correction);
    auto* harfbuzz_font = static_cast<Gfx::Font const*>(font)->harfbuzz_font();
    Vector<hb_ot_math_glyph_part_t, 8> parts;
    parts.resize(capacity);
    unsigned count = capacity;
    hb_position_t italics_correction = 0;
    auto total = hb_ot_math_get_glyph_assembly(harfbuzz_font, glyph_id, math_stretch_direction(horizontal), 0, &count, parts.data(), &italics_correction);
    for (size_t index = 0; index < count; ++index) {
        out_parts[index] = {
            .glyph_id = parts[index].glyph,
            .start_connector_length = Gfx::harfbuzz_position_to_pixels(parts[index].start_connector_length),
            .end_connector_length = Gfx::harfbuzz_position_to_pixels(parts[index].end_connector_length),
            .full_advance = Gfx::harfbuzz_position_to_pixels(parts[index].full_advance),
            .is_extender = (parts[index].flags & HB_OT_MATH_GLYPH_PART_FLAG_EXTENDER) != 0,
        };
    }
    *out_italics_correction = Gfx::harfbuzz_position_to_pixels(italics_correction);
    return total;
}

extern "C" void ladybird_gfx_font_ref(void const* font)
{
    VERIFY(font);
    static_cast<Gfx::Font const*>(font)->ref();
}

extern "C" void ladybird_gfx_font_unref(void const* font)
{
    VERIFY(font);
    static_cast<Gfx::Font const*>(font)->unref();
}
