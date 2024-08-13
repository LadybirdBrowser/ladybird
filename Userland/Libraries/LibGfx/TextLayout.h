/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Forward.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace Gfx {

inline bool should_paint_as_space(u32 code_point)
{
    return is_ascii_space(code_point) || code_point == 0xa0;
}

enum class IncludeLeftBearing {
    Yes,
    No
};

struct DrawGlyph {
    FloatPoint position;
    u32 glyph_id;

    void translate_by(FloatPoint const& delta)
    {
        position.translate_by(delta);
    }
};

struct DrawEmoji {
    FloatPoint position;
    Gfx::Bitmap const* emoji;

    void translate_by(FloatPoint const& delta)
    {
        position.translate_by(delta);
    }
};

using DrawGlyphOrEmoji = Variant<DrawGlyph, DrawEmoji>;

class GlyphRun : public RefCounted<GlyphRun> {
public:
    GlyphRun(Vector<Gfx::DrawGlyphOrEmoji>&& glyphs, NonnullRefPtr<Font> font)
        : m_glyphs(move(glyphs))
        , m_font(move(font))
    {
    }

    [[nodiscard]] Font const& font() const { return m_font; }
    [[nodiscard]] Vector<Gfx::DrawGlyphOrEmoji> const& glyphs() const { return m_glyphs; }
    [[nodiscard]] Vector<Gfx::DrawGlyphOrEmoji>& glyphs() { return m_glyphs; }
    [[nodiscard]] bool is_empty() const { return m_glyphs.is_empty(); }

    void append(Gfx::DrawGlyphOrEmoji glyph) { m_glyphs.append(glyph); }

private:
    Vector<Gfx::DrawGlyphOrEmoji> m_glyphs;
    NonnullRefPtr<Font> m_font;
};

void for_each_glyph_position(FloatPoint baseline_start, Utf8View string, Gfx::Font const& font, Function<void(DrawGlyphOrEmoji const&)> callback, IncludeLeftBearing include_left_bearing = IncludeLeftBearing::No, Optional<float&> width = {});

}
