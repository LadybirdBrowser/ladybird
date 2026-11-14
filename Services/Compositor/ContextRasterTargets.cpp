/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ContextRasterTargets.h>
#include <Compositor/FontSkia.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <core/SkFont.h>
#include <core/SkFontMetrics.h>
#include <core/SkFontTypes.h>

namespace Compositor {

ContextRasterTargets::ContextRasterTargets(OwnedCanvasSurfaces canvas_surfaces)
    : m_canvas_surfaces(canvas_surfaces)
{
}

void ContextRasterTargets::apply_resource_transaction(Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    m_raster_cache.evict(m_resource_storage.apply_transaction(move(resource_transaction)));
}

Optional<OverlayLabel> ContextRasterTargets::shape_overlay_label(Optional<StringView> font_family, double device_pixel_ratio, StringView text)
{
    RefPtr<Gfx::Font> font;
    if (font_family.has_value())
        font = Gfx::FontDatabase::the().get(MUST(FlyString::from_utf8(*font_family)), 12, 400, Gfx::FontWidth::Normal, 0);
    if (!font)
        font = Gfx::FontDatabase::the().get("SerenitySans"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0);
    if (!font)
        return {};
    auto skia_font = Compositor::skia_font(*font, static_cast<float>(device_pixel_ratio));
    if (!skia_font.has_value())
        return {};

    OverlayLabel label;
    auto glyph_count = skia_font->countText(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8);
    Vector<SkGlyphID> glyphs;
    glyphs.resize(glyph_count);
    skia_font->textToGlyphs(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8, { glyphs.data(), glyphs.size() });
    label.glyph_x_positions.resize(glyph_count);
    skia_font->getXPos({ glyphs.data(), glyphs.size() }, { label.glyph_x_positions.data(), label.glyph_x_positions.size() });
    label.glyph_ids.ensure_capacity(glyph_count);
    for (auto glyph : glyphs)
        label.glyph_ids.unchecked_append(glyph);
    label.width = skia_font->measureText(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8);
    SkFontMetrics font_metrics;
    skia_font->getMetrics(&font_metrics);
    label.ascent = font_metrics.fAscent;
    label.descent = font_metrics.fDescent;
    label.font_ascent = font->pixel_metrics().ascent;
    label.font_id = m_overlay_resource_storage.add_font(*font).value();
    return label;
}

RefPtr<PresentableSurface> ContextRasterTargets::next_presentable_surface(u32 pool, Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto surface = m_presentable_surface_pools.ensure(pool).surface_for_next_content(size, skia_backend_context);
    if (surface.is_error())
        return nullptr;
    return surface.release_value();
}

RefPtr<Media::VideoFrame> ContextRasterTargets::current_video_frame(Compositing::VideoSinkResourceId video_sink_id) const
{
    auto sink = m_resource_storage.video_sink(video_sink_id);
    if (!sink)
        return {};
    return sink->current_frame();
}

bool ContextRasterTargets::draws_video_sink(Media::VideoSinkHandle handle) const
{
    for (auto const& [id, sink_handle] : m_resource_storage.video_sink_handles()) {
        if (sink_handle == handle)
            return true;
    }
    return false;
}

}
