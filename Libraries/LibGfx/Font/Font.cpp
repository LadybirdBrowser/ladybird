/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16String.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontTable.h>
#include <RustFFI.h>

#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

// NB: Rust reads the features of a font as these.
static_assert(sizeof(Gfx::FFI::FfiShapeFeature) == sizeof(Gfx::ShapeFeature));
static_assert(alignof(Gfx::FFI::FfiShapeFeature) == alignof(Gfx::ShapeFeature));
static_assert(offsetof(Gfx::FFI::FfiShapeFeature, tag) == offsetof(Gfx::ShapeFeature, tag));
static_assert(offsetof(Gfx::FFI::FfiShapeFeature, value) == offsetof(Gfx::ShapeFeature, value));
static_assert(offsetof(Gfx::FFI::FfiShapeFeature, set_by_feature_settings) == offsetof(Gfx::ShapeFeature, set_by_feature_settings));
static_assert(IsTriviallyCopyable<Gfx::ShapeFeature>);

extern "C" {
void ladybird_gfx_font_snapshot(void const*, Gfx::FFI::FfiFontSnapshot*);
bool ladybird_gfx_font_is_emoji_font(void const*);
void ladybird_gfx_font_ref(void const*);
void ladybird_gfx_font_unref(void const*);
}

namespace Gfx {

static Atomic<u64> s_next_id { 1 };

Font::Font(NonnullRefPtr<Typeface const> typeface, float point_width, float point_height, FontVariationSettings const variations, ShapeFeatures const& features, FontSynthesis synthesis)
    : m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_typeface(move(typeface))
    , m_point_width(point_width)
    , m_point_height(point_height)
    , m_font_variation_settings(move(variations))
    , m_shape_features(features)
    , m_synthesis(synthesis)
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
    metrics.line_gap = max(0.0f, font_units_to_pixels(vertical_metrics.line_gap + hb_ot_metrics_get_variation(m_harfbuzz_font, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP)));

#ifdef AK_OS_LINUX
    if (m_typeface->system_font_identifier().has_value()) {
        // INTEROP: Use FreeType's enclosing pixel sizes for Linux system fonts with legacy horizontal metrics.
        // https://learn.microsoft.com/en-us/typography/opentype/spec/os2#fsselection
        static constexpr u16 USE_TYPO_METRICS = 1 << 7;
        auto* face = hb_font_get_face(m_harfbuzz_font);
        FontTable os2 { face, FourCC { "OS/2" } };
        FontTable hhea { face, FourCC { "hhea" } };
        auto requested_typo_metrics = os2.contains(62, 2) && (os2.u16_at(62) & USE_TYPO_METRICS);
        auto hhea_has_metrics = hhea.contains(4, 4) && (hhea.i16_at(4) != 0 || hhea.i16_at(6) != 0);
        if (!requested_typo_metrics && hhea_has_metrics) {
            metrics.ascent = AK::ceil(AK::round(metrics.ascent * text_shaping_resolution) / text_shaping_resolution);
            metrics.descent = AK::ceil(AK::round(metrics.descent * text_shaping_resolution) / text_shaping_resolution);
        }
    }
#endif

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

    // https://drafts.csswg.org/css-values-4/#cap
    // In the cases where it is impossible or impractical to determine the cap-height, the font's ascent must be used.
    // NB: A font whose OS/2 table doesn't give its cap height has it measured from the top of its "H" glyph instead.
    metrics.cap_height = metrics.ascent;
    hb_position_t cap_height = 0;
    if (hb_ot_metrics_get_position(m_harfbuzz_font, HB_OT_METRICS_TAG_CAP_HEIGHT, &cap_height) && cap_height > 0) {
        metrics.cap_height = static_cast<float>(cap_height) / text_shaping_resolution;
    } else if (y_scale > 0 && hb_font_get_nominal_glyph(m_harfbuzz_font, 'H', &glyph_id) && hb_font_get_glyph_extents(m_harfbuzz_font, glyph_id, &extents) && extents.y_bearing > 0) {
        metrics.cap_height = static_cast<float>(extents.y_bearing) * m_pixel_size / static_cast<float>(y_scale);
    }

    // https://drafts.csswg.org/css-values-4/#ch
    // The advance of the glyph that shaping uses for "0". In the cases where it is impossible or impractical to
    // determine the measure of the "0" glyph, it must be assumed to be 0.5em wide.
    metrics.advance_of_ascii_zero = m_pixel_size / 2;
    if (hb_font_get_nominal_glyph(m_harfbuzz_font, '0', &glyph_id))
        metrics.advance_of_ascii_zero = static_cast<float>(hb_font_get_glyph_h_advance(m_harfbuzz_font, glyph_id)) / text_shaping_resolution;

    auto* face = hb_font_get_face(m_harfbuzz_font);
    auto design_units_to_pixels = m_pixel_size / m_typeface->units_per_em();
    // https://learn.microsoft.com/en-us/typography/opentype/spec/os2#xavgcharwidth
    if (FontTable os2 { face, FourCC { "OS/2" } }; os2.contains(2, 2))
        metrics.average_character_advance = max<i16>(0, os2.i16_at(2)) * design_units_to_pixels;
    // https://learn.microsoft.com/en-us/typography/opentype/spec/hhea
    if (FontTable hhea { face, FourCC { "hhea" } }; hhea.contains(10, 2))
        metrics.maximum_character_advance = hhea.u16_at(10) * design_units_to_pixels;

    return metrics;
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

// What shaping in Rust needs of the font: its immutable HarfBuzz font and its features, which it lends for as long as
// it lives.
static FFI::FfiShapingFont shaping_font_of(Font const& font)
{
    auto const& features = font.features();
    return {
        .harfbuzz_font = font.harfbuzz_font(),
        .features = reinterpret_cast<FFI::FfiShapeFeature const*>(features.data()),
        .feature_count = features.size(),
    };
}

float Font::width(Utf16View const& view) const
{
    // NB: Shaping takes ASCII text as Latin script and left to right without asking HarfBuzz, so the storage matters.
    if (view.has_latin1_storage()) {
        auto storage = view.has_ascii_storage() ? FFI::TextStorage::Ascii : FFI::TextStorage::Latin1;
        return FFI::ladybird_gfx_font_measure_text_width(shaping_font_of(*this), storage, view.latin1_span().data(), view.length_in_code_units());
    }
    return FFI::ladybird_gfx_font_measure_text_width(shaping_font_of(*this), FFI::TextStorage::Utf16, view.utf16_span().data(), view.length_in_code_units());
}

NonnullRefPtr<Font> Font::invisible_variant() const
{
    return m_typeface->invisible_font(m_point_height, m_font_variation_settings, m_shape_features, m_synthesis);
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
    // NB: HarfBuzz slants the glyph outlines and extents it reports, so that text is measured as it's drawn.
    if (m_synthesis.oblique_angle != 0)
        hb_font_set_synthetic_slant(font, AK::tan(AK::to_radians(m_synthesis.oblique_angle)));
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

}

extern "C" void ladybird_gfx_font_snapshot(void const* font, Gfx::FFI::FfiFontSnapshot* out_snapshot)
{
    VERIFY(font);
    VERIFY(out_snapshot);
    auto const& typed_font = *static_cast<Gfx::Font const*>(font);
    auto const& metrics = typed_font.pixel_metrics();
    auto bounding_box_of_all_glyphs = typed_font.typeface().bounding_box_in_font_units();
    *out_snapshot = {
        .id = typed_font.id(),
        .shaping = Gfx::shaping_font_of(typed_font),
        .ascent = metrics.ascent,
        .descent = metrics.descent,
        .line_gap = metrics.line_gap,
        .x_height = metrics.x_height,
        .cap_height = metrics.cap_height,
        .zero_advance = metrics.advance_of_ascii_zero,
        .average_character_advance = metrics.average_character_advance,
        .maximum_character_advance = metrics.maximum_character_advance,
        .pixel_size = typed_font.pixel_size(),
        .point_size = typed_font.point_size(),
        .has_bounding_box_of_all_glyphs = !bounding_box_of_all_glyphs.is_empty(),
        .bounding_box_of_all_glyphs = {
            .x_min = bounding_box_of_all_glyphs.x_min,
            .y_min = bounding_box_of_all_glyphs.y_min,
            .x_max = bounding_box_of_all_glyphs.x_max,
            .y_max = bounding_box_of_all_glyphs.y_max,
            .units_per_em = bounding_box_of_all_glyphs.units_per_em,
        },
        .synthetic_slant = hb_font_get_synthetic_slant(typed_font.harfbuzz_font()),
        .has_synthetic_bold = typed_font.synthesis().bold,
        .is_invisible = typed_font.is_invisible(),
        .glyph_count = typed_font.typeface().glyph_count(),
    };
}

extern "C" bool ladybird_gfx_font_is_emoji_font(void const* font)
{
    VERIFY(font);
    return static_cast<Gfx::Font const*>(font)->is_emoji_font();
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
