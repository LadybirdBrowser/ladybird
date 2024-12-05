/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TextLayout.h"
#include <AK/TypeCasts.h>
#include <LibGfx/Point.h>
#include <harfbuzz/hb.h>

namespace Gfx {

RefPtr<GlyphRun> shape_text(FloatPoint baseline_start, float letter_spacing, Utf8View string, Gfx::Font const& font, GlyphRun::TextType text_type, ShapeFeatures const& features)
{
    hb_buffer_t* buffer = hb_buffer_create();
    ScopeGuard destroy_buffer = [&]() { hb_buffer_destroy(buffer); };
    hb_buffer_add_utf8(buffer, reinterpret_cast<char const*>(string.bytes()), string.byte_length(), 0, -1);
    hb_buffer_guess_segment_properties(buffer);

    u32 glyph_count;
    auto* glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    Vector<hb_glyph_info_t> const input_glyph_info({ glyph_info, glyph_count });

    auto* hb_font = font.harfbuzz_font();
    hb_feature_t const* hb_features_data = nullptr;
    Vector<hb_feature_t> hb_features;
    if (!features.is_empty()) {
        hb_features.ensure_capacity(features.size());
        for (auto const& feature : features) {
            hb_features.append({
                .tag = HB_TAG(feature.tag[0], feature.tag[1], feature.tag[2], feature.tag[3]),
                .value = feature.value,
                .start = 0,
                .end = HB_FEATURE_GLOBAL_END,
            });
        }
        hb_features_data = hb_features.data();
    }

    hb_shape(hb_font, buffer, hb_features_data, features.size());

    glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    auto* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    Vector<Gfx::DrawGlyph> glyph_run;
    FloatPoint point = baseline_start;
    for (size_t i = 0; i < glyph_count; ++i) {

        auto position = point
            - FloatPoint { 0, font.pixel_metrics().ascent }
            + FloatPoint { positions[i].x_offset, positions[i].y_offset } / text_shaping_resolution;
        glyph_run.append({ position, glyph_info[i].codepoint });
        point += FloatPoint { positions[i].x_advance, positions[i].y_advance } / text_shaping_resolution;

        // don't apply spacing to last glyph
        // https://drafts.csswg.org/css-text/#example-7880704e
        if (i != (glyph_count - 1))
            point.translate_by(letter_spacing, 0);
    }

    hb_buffer_reset(buffer);

    return adopt_ref(*new Gfx::GlyphRun(move(glyph_run), font, text_type, point.x()));
}

float measure_text_width(Utf8View const& string, Gfx::Font const& font, ShapeFeatures const& features)
{
    auto glyph_run = shape_text({}, 0, string, font, GlyphRun::TextType::Common, features);
    return glyph_run->width();
}

}
