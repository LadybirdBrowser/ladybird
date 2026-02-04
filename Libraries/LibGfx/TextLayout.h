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
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShapeFeature.h>

class SkTextBlob;

namespace Gfx {

struct DrawGlyph {
    FloatPoint position;
    size_t length_in_code_units { 0 };
    float glyph_width { 0.0 };
    u32 glyph_id { 0 };
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
    [[nodiscard]] bool is_empty() const { return m_glyphs.is_empty(); }
    [[nodiscard]] float width() const { return m_width; }

    [[nodiscard]] NonnullRefPtr<GlyphRun> slice(size_t start, size_t length) const;

    void ensure_text_blob(float scale) const;

    FloatRect cached_blob_bounds() const;
    SkTextBlob* cached_skia_text_blob() const;

private:
    Vector<DrawGlyph> m_glyphs;
    NonnullRefPtr<Font const> m_font;
    TextType m_text_type;
    float m_width { 0 };

    struct CachedTextBlob;
    mutable OwnPtr<CachedTextBlob> m_cached_text_blob;
};

NonnullRefPtr<GlyphRun> shape_text(FloatPoint baseline_start, float letter_spacing, Utf16View const&, Gfx::Font const& font, GlyphRun::TextType);
Vector<NonnullRefPtr<GlyphRun>> shape_text(FloatPoint baseline_start, Utf16View const&, FontCascadeList const&);
float measure_text_width(Utf16View const&, Gfx::Font const& font);

}
