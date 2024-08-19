/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TextLayout.h"
#include "Font/Emoji.h"
#include <AK/TypeCasts.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibUnicode/Emoji.h>
#include <harfbuzz/hb.h>

namespace Gfx {

static DrawGlyphOrEmoji construct_glyph_or_emoji(size_t index, FloatPoint const& position, Gfx::Font const&, Span<hb_glyph_info_t const> glyph_info, Span<hb_glyph_info_t const>)
{
    return DrawGlyph {
        .position = position,
        .glyph_id = glyph_info[index].codepoint,
    };
}

void for_each_glyph_position(FloatPoint baseline_start, Utf8View string, Gfx::Font const& font, Function<void(DrawGlyphOrEmoji const&)> callback, Optional<float&> width)
{
    hb_buffer_t* buffer = hb_buffer_create();
    ScopeGuard destroy_buffer = [&]() { hb_buffer_destroy(buffer); };
    hb_buffer_add_utf8(buffer, reinterpret_cast<char const*>(string.bytes()), string.byte_length(), 0, -1);
    hb_buffer_guess_segment_properties(buffer);

    u32 glyph_count;
    auto* glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    Vector<hb_glyph_info_t> const input_glyph_info({ glyph_info, glyph_count });
    if (input_glyph_info.is_empty())
        return;

    auto* hb_font = font.harfbuzz_font();
    hb_shape(hb_font, buffer, nullptr, 0);

    glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    auto* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    FloatPoint point = baseline_start;
    for (size_t i = 0; i < glyph_count; ++i) {
        auto position = point
            - FloatPoint { 0, font.pixel_metrics().ascent }
            + FloatPoint { positions[i].x_offset, positions[i].y_offset } / text_shaping_resolution;
        callback(construct_glyph_or_emoji(i, position, font, { glyph_info, glyph_count }, input_glyph_info.span()));
        point += FloatPoint { positions[i].x_advance, positions[i].y_advance } / text_shaping_resolution;
    }

    if (width.has_value())
        *width = point.x();
}

}
