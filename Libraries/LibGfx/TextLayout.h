/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShapeFeature.h>

namespace Gfx {

struct DrawGlyph {
    FloatPoint position;
    size_t length_in_code_units { 0 };
    float glyph_width { 0.0 };
    u32 glyph_id { 0 };
    u32 cluster_start { 0 };
    bool should_paint { true };
    bool is_rtl { false };
};

// Conservative bounds of the painted glyphs in device pixels, relative to the run's baseline origin (glyph
// positions scaled by `scale`, with the font ascent added to y). The bounding box of the glyph origins is
// expanded by the font's overall glyph bounding box, falling back to tight per-glyph extents when the font
// doesn't record one.
[[nodiscard]] FloatRect glyph_run_bounding_box(Font const&, ReadonlySpan<DrawGlyph>, float scale);

// Pairs of [start, end] device-pixel x values, relative to the run's baseline origin, where glyph ink intersects
// the horizontal band [y_top, y_bottom]. One pair per painted glyph that has ink inside the band, in run order.
[[nodiscard]] Vector<float> glyph_run_glyph_intercepts(Font const&, ReadonlySpan<DrawGlyph>, float scale, float y_top, float y_bottom);

float measure_text_width(Utf16View const&, Font const& font, float letter_spacing = 0.f);

}
