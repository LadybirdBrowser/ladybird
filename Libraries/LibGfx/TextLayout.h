/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
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
    bool should_paint { true };
};

struct TrailingWhitespace {
    size_t length_in_code_units { 0 };
    float advance { 0 };
};

class GlyphRun : public AtomicRefCounted<GlyphRun> {
public:
    enum class TextType {
        Common,
        ContextDependent,
        EndPadding,
        Ltr,
        Rtl,
    };

    GlyphRun(Vector<DrawGlyph>&& glyphs, NonnullRefPtr<Font const> font, TextType text_type, float width);
    ~GlyphRun();

    [[nodiscard]] Font const& font() const { return m_font; }
    [[nodiscard]] TextType text_type() const { return m_text_type; }
    [[nodiscard]] Vector<DrawGlyph> const& glyphs() const { return m_glyphs; }
    [[nodiscard]] Vector<DrawGlyph>& glyphs() { return m_glyphs; }
    [[nodiscard]] float width() const { return m_width; }

    [[nodiscard]] NonnullRefPtr<GlyphRun> slice(size_t start, size_t length) const;

private:
    Vector<DrawGlyph> m_glyphs;
    NonnullRefPtr<Font const> m_font;
    TextType m_text_type;
    float m_width { 0 };
};

// Conservative bounds of the painted glyphs in device pixels, relative to the run's baseline origin (glyph
// positions scaled by `scale`, with the font ascent added to y). The bounding box of the glyph origins is
// expanded by the font's overall glyph bounding box, falling back to tight per-glyph extents when the font
// doesn't record one.
[[nodiscard]] FloatRect glyph_run_bounding_box(Font const&, ReadonlySpan<DrawGlyph>, float scale);

// Pairs of [start, end] device-pixel x values, relative to the run's baseline origin, where glyph ink intersects
// the horizontal band [y_top, y_bottom]. One pair per painted glyph that has ink inside the band, in run order.
[[nodiscard]] Vector<float> glyph_run_glyph_intercepts(Font const&, ReadonlySpan<DrawGlyph>, float scale, float y_top, float y_bottom);

NonnullRefPtr<GlyphRun> shape_text(FloatPoint baseline_start, float letter_spacing, float word_spacing, Utf16View const&, Gfx::Font const& font, GlyphRun::TextType, TrailingWhitespace* = nullptr);
Vector<NonnullRefPtr<GlyphRun>> shape_text(FloatPoint baseline_start, Utf16View const&, FontCascadeList const&, float letter_spacing = 0.f);
float measure_text_width(Utf16View const&, Font const& font, float letter_spacing = 0.f);

}
