/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibGfx/Point.h>
#include <LibGfx/TextLayout.h>
#include <harfbuzz/hb.h>

namespace Gfx {

Vector<NonnullRefPtr<GlyphRun>> shape_text(FloatPoint baseline_start, Utf16View const& string, FontCascadeList const& font_cascade_list)
{
    if (string.is_empty())
        return {};

    Vector<NonnullRefPtr<GlyphRun>> runs;

    auto it = string.begin();
    auto substring_begin_offset = string.iterator_offset(it);
    Font const* last_font = &font_cascade_list.font_for_code_point(*it);
    FloatPoint last_position = baseline_start;

    auto add_run = [&runs, &last_position](Utf16View const& string, Font const& font) {
        auto run = shape_text(last_position, 0, string, font, GlyphRun::TextType::Common, {});
        last_position.translate_by(run->width(), 0);
        runs.append(*run);
    };

    while (it != string.end()) {
        auto code_point = *it;
        auto const* font = &font_cascade_list.font_for_code_point(code_point);
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

static hb_buffer_t* setup_text_shaping(Utf16View const& string, Font const& font, ShapeFeatures const& features)
{
    static hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_reset(buffer);

    if (string.has_ascii_storage())
        hb_buffer_add_utf8(buffer, string.ascii_span().data(), string.length_in_code_units(), 0, -1);
    else
        hb_buffer_add_utf16(buffer, reinterpret_cast<u16 const*>(string.utf16_span().data()), string.length_in_code_units(), 0, -1);

    hb_buffer_guess_segment_properties(buffer);

    auto* hb_font = font.harfbuzz_font();
    hb_feature_t const* hb_features_data = nullptr;
    Vector<hb_feature_t> hb_features;
    if (!features.is_empty()) {
        hb_features.ensure_capacity(features.size());
        for (auto const& feature : features) {
            hb_features.unchecked_append({
                .tag = HB_TAG(feature.tag[0], feature.tag[1], feature.tag[2], feature.tag[3]),
                .value = feature.value,
                .start = 0,
                .end = HB_FEATURE_GLOBAL_END,
            });
        }
        hb_features_data = hb_features.data();
    }

    hb_shape(hb_font, buffer, hb_features_data, features.size());

    return buffer;
}

NonnullRefPtr<GlyphRun> shape_text(FloatPoint baseline_start, float letter_spacing, Utf16View const& string, Font const& font, GlyphRun::TextType text_type, ShapeFeatures const& features)
{
    auto* buffer = setup_text_shaping(string, font, features);

    u32 glyph_count;
    auto const* glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    auto const* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    Vector<DrawGlyph> glyph_run;
    glyph_run.ensure_capacity(glyph_count);
    FloatPoint point = baseline_start;

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

    for (size_t i = 0; i < glyph_count; ++i) {
        auto position = point
            - FloatPoint { 0, font.pixel_metrics().ascent }
            + FloatPoint { positions[i].x_offset, positions[i].y_offset } / text_shaping_resolution;

        glyph_run.unchecked_append({
            .position = position,
            .length_in_code_units = glyph_length_in_code_units(i),
            .glyph_width = positions[i].x_advance / text_shaping_resolution,
            .glyph_id = glyph_info[i].codepoint,
        });

        point += FloatPoint { positions[i].x_advance, positions[i].y_advance } / text_shaping_resolution;

        // NOTE: The spec says that we "really should not" apply letter-spacing to the trailing edge of a line but
        //       other browsers do so we will as well. https://drafts.csswg.org/css-text/#example-7880704e
        point.translate_by(letter_spacing, 0);
    }

    return adopt_ref(*new GlyphRun(move(glyph_run), font, text_type, point.x() - baseline_start.x()));
}

float measure_text_width(Utf16View const& string, Font const& font, ShapeFeatures const& features)
{
    auto* buffer = setup_text_shaping(string, font, features);

    u32 glyph_count;
    auto const* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    hb_position_t point_x = 0;
    for (size_t i = 0; i < glyph_count; ++i)
        point_x += positions[i].x_advance;

    return point_x / text_shaping_resolution;
}

}
