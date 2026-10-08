/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/CompositedContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibGfx/Forward.h>

class GrDirectContext;
class SkColorFilter;
class SkImage;
class SkImageFilter;
class SkPaint;
template<typename T>
class sk_sp;

namespace Compositor {

// The color filter force-dark runs classified images through: the same Oklab lightness inversion the solid colors
// take — as a Skia runtime effect. Exposed so a benchmark can time the shipped effect, rather than a copy of it.
sk_sp<SkColorFilter> force_dark_image_color_filter();

class DisplayListPlayerSkia final : public Compositing::DisplayListPlayer {
public:
    AK_ALLOC_WITH_KMALLOC;

    DisplayListPlayerSkia();
    explicit DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext>);
    ~DisplayListPlayerSkia();

    void execute(
        Compositing::DisplayList const&,
        Compositing::AccumulatedVisualContextTree const&,
        Compositing::DisplayListResourceStorage const&,
        DisplayListRasterCache&,
        Compositing::ScrollStateSnapshot const&,
        RefPtr<Gfx::PaintingSurface>,
        Compositing::CanvasSurfaceRegistry const*,
        Compositing::CompositedContextResolver const*);

    void flush(Gfx::PaintingSurface&) override;
    void flush_async(Gfx::PaintingSurface&, Function<void()>&&);
    void paint_scrollbar(Gfx::PaintingSurface&, Compositing::PaintScrollBar const&);

private:
#define DECLARE_PLAY_COMMAND(command_type) \
    void play_command(Compositing::command_type const&) override;
    ENUMERATE_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    void set_matrix(Gfx::FloatMatrix4x4 const&) override;
    Gfx::FloatMatrix4x4 canvas_matrix() const override;
    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    void push_clip(Compositing::ReplayClip const&) override;
    void push_clip_path(Gfx::Path const&, Gfx::WindingRule) override;
    void push_transform(Gfx::AffineTransform const&) override;
    void push_layer(Compositing::ReplayLayer const&) override;
    void push_mask(Compositing::ReplayMask const&) override;
    void pop_mask(Compositing::ReplayMask const&, Compositing::EffectNodeIndex) override;
    void pop() override;
    void push_device_space_plane_clip(Gfx::Path const&) override;

    void clip_path(Gfx::Path const&, Gfx::WindingRule, bool anti_aliased);

    SkPaint paint_style_to_skia_paint(Compositing::DisplayListPaintStyle const&, Gfx::FloatRect const& bounding_rect);
    sk_sp<SkImageFilter> layer_image_filter(Compositing::ReplayLayer const&);
    sk_sp<SkImageFilter> backdrop_image_filter(Compositing::ReplayLayer const&, bool limited_to_region);
    sk_sp<SkImageFilter> image_filter_from_bytes(ReadonlyBytes);
    Gfx::Path path_from_data(Compositing::DisplayListDataSpan) const;
    sk_sp<SkImage> rasterize_records_into_tile(ReadonlyBytes tile_records, Gfx::IntRect tile_rect);
    ReadonlySpan<Color> gradient_colors(Compositing::DisplayListGradientColorStops) const;
    ReadonlySpan<float> gradient_positions(Compositing::DisplayListGradientColorStops) const;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Compositing::CompositedContextResolver const* m_composited_context_resolver { nullptr };
    DisplayListRasterCache* m_raster_cache { nullptr };
    DisplayListRasterCache& raster_cache() const { return *m_raster_cache; }

    // Layer filters are built from their bytes once per frame node and kept until the visual
    // context tree's structure changes, so a replayed frame does not rebuild them.
    struct LayerImageFilterCache;
    NonnullOwnPtr<LayerImageFilterCache> m_layer_image_filter_cache;
};

}
