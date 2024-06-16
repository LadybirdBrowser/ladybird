/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibAccelGfx/GlyphAtlas.h>
#include <LibGfx/Painter.h>

namespace AccelGfx {

GlyphAtlas& GlyphAtlas::the()
{
    static OwnPtr<GlyphAtlas> s_the;
    if (!s_the)
        s_the = make<GlyphAtlas>();
    return *s_the;
}

void GlyphAtlas::update(HashMap<Gfx::Font const*, HashTable<u32>> const& unique_glyphs)
{
    auto need_to_rebuild_texture = false;
    HashMap<GlyphsTextureKey, NonnullRefPtr<Gfx::Bitmap>> glyph_bitmaps;
    for (auto const& [font, code_points] : unique_glyphs) {
        for (auto const& code_point : code_points) {
            auto glyph = font->glyph(code_point);
            if (!glyph.has_value())
                continue;
            auto atlas_key = GlyphsTextureKey { font, code_point };
            if (!m_glyphs_texture_map.contains(atlas_key))
                need_to_rebuild_texture = true;
            glyph_bitmaps.set(atlas_key, *glyph->bitmap());
        }
    }

    if (!need_to_rebuild_texture || glyph_bitmaps.is_empty())
        return;

    m_glyphs_texture_map.clear();

    Vector<GlyphsTextureKey> glyphs_sorted_by_height;
    glyphs_sorted_by_height.ensure_capacity(glyph_bitmaps.size());
    for (auto const& [atlas_key, bitmap] : glyph_bitmaps) {
        glyphs_sorted_by_height.append(atlas_key);
    }
    quick_sort(glyphs_sorted_by_height, [&](auto const& a, auto const& b) {
        auto const* bitmap_a = *glyph_bitmaps.get(a);
        auto const* bitmap_b = *glyph_bitmaps.get(b);
        return bitmap_a->height() > bitmap_b->height();
    });

    int current_x = 0;
    int current_y = 0;
    int row_height = 0;
    int const texture_width = 512;
    int const padding = 1;
    for (auto const& glyphs_texture_key : glyphs_sorted_by_height) {
        auto const* bitmap = *glyph_bitmaps.get(glyphs_texture_key);
        if (current_x + bitmap->width() > texture_width) {
            current_x = 0;
            current_y += row_height + padding;
            row_height = 0;
        }
        m_glyphs_texture_map.set(glyphs_texture_key, { current_x, current_y, bitmap->width(), bitmap->height() });
        current_x += bitmap->width() + padding;
        row_height = max(row_height, bitmap->height());
    }

    auto glyphs_texture_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { texture_width, current_y + row_height }));
    auto glyphs_texture_painter = Gfx::Painter(*glyphs_texture_bitmap);
    for (auto const& [glyphs_texture_key, glyph_bitmap] : glyph_bitmaps) {
        auto rect = m_glyphs_texture_map.get(glyphs_texture_key).value();
        glyphs_texture_painter.blit({ rect.x(), rect.y() }, glyph_bitmap, glyph_bitmap->rect());
    }

    GL::upload_texture_data(m_texture, *glyphs_texture_bitmap);
}

Optional<Gfx::IntRect> GlyphAtlas::get_glyph_rect(Gfx::Font const* font, u32 code_point) const
{
    auto atlas_key = GlyphsTextureKey { font, code_point };
    return m_glyphs_texture_map.get(atlas_key);
}

}
