/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Typeface.h>

namespace WOFF {

class Typeface : public Gfx::Typeface {
    AK_MAKE_NONCOPYABLE(Typeface);

public:
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_resource(Core::Resource const&, unsigned index = 0);
    static ErrorOr<NonnullRefPtr<Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes bytes, unsigned index = 0);

    virtual Gfx::ScaledFontMetrics metrics(float x_scale, float y_scale) const override { return m_input_font->metrics(x_scale, y_scale); }
    virtual Gfx::ScaledGlyphMetrics glyph_metrics(u32 glyph_id, float x_scale, float y_scale, float point_width, float point_height) const override { return m_input_font->glyph_metrics(glyph_id, x_scale, y_scale, point_width, point_height); }
    virtual float glyph_advance(u32 glyph_id, float x_scale, float y_scale, float point_width, float point_height) const override { return m_input_font->glyph_advance(glyph_id, x_scale, y_scale, point_width, point_height); }
    virtual float glyphs_horizontal_kerning(u32 left_glyph_id, u32 right_glyph_id, float x_scale) const override { return m_input_font->glyphs_horizontal_kerning(left_glyph_id, right_glyph_id, x_scale); }
    virtual RefPtr<Gfx::Bitmap> rasterize_glyph(u32 glyph_id, float x_scale, float y_scale, Gfx::GlyphSubpixelOffset subpixel_offset) const override { return m_input_font->rasterize_glyph(glyph_id, x_scale, y_scale, subpixel_offset); }
    virtual bool append_glyph_path_to(Gfx::Path& path, u32 glyph_id, float x_scale, float y_scale) const override { return m_input_font->append_glyph_path_to(path, glyph_id, x_scale, y_scale); }

    virtual u32 glyph_count() const override { return m_input_font->glyph_count(); }
    virtual u16 units_per_em() const override { return m_input_font->units_per_em(); }
    virtual u32 glyph_id_for_code_point(u32 code_point) const override { return m_input_font->glyph_id_for_code_point(code_point); }
    virtual String family() const override { return m_input_font->family(); }
    virtual String variant() const override { return m_input_font->variant(); }
    virtual u16 weight() const override { return m_input_font->weight(); }
    virtual u16 width() const override { return m_input_font->width(); }
    virtual u8 slope() const override { return m_input_font->slope(); }
    virtual bool is_fixed_width() const override { return m_input_font->is_fixed_width(); }
    virtual bool has_color_bitmaps() const override { return m_input_font->has_color_bitmaps(); }

private:
    Typeface(NonnullRefPtr<Gfx::Typeface const> input_font, ByteBuffer input_font_buffer)
        : m_input_font_buffer(move(input_font_buffer))
        , m_input_font(move(input_font))
    {
    }

    ByteBuffer m_input_font_buffer;
    NonnullRefPtr<Gfx::Typeface const> m_input_font;
};

}
