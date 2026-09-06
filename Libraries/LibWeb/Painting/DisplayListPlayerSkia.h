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
#include <LibGfx/Forward.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>

class GrDirectContext;
class SkColorFilter;
class SkImageFilter;
class SkPaint;
template<typename T>
class sk_sp;

namespace Web::Painting {

// The color filter force-dark runs classified images through: the same Oklab lightness inversion the solid colors
// take — as a Skia runtime effect. Exposed so a benchmark can time the shipped effect, rather than a copy of it.
WEB_API sk_sp<SkColorFilter> force_dark_image_color_filter();

class WEB_API DisplayListPlayerSkia final : public DisplayListPlayer {
public:
    using CompositedContextResolver = Function<RefPtr<Gfx::PaintingSurface>(Web::Compositor::CompositorContextId)>;

    DisplayListPlayerSkia();
    explicit DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext>);
    ~DisplayListPlayerSkia();

    using DisplayListPlayer::execute;
    void execute(
        DisplayList const&,
        AccumulatedVisualContextTree const&,
        DisplayListResourceStorage const&,
        ScrollStateSnapshot const&,
        RefPtr<Gfx::PaintingSurface>,
        CanvasSurfaceRegistry const*,
        CompositedContextResolver const*);

    void flush(Gfx::PaintingSurface&) override;
    void flush_async(Gfx::PaintingSurface&, Function<void()>&&);
    void paint_scrollbar(Gfx::PaintingSurface&, PaintScrollBar const&);

private:
#define DECLARE_PLAY_COMMAND(command_type, player_method) \
    void play_command(command_type const&) override;
    ENUMERATE_DISPLAY_LIST_COMMANDS(DECLARE_PLAY_COMMAND)
#undef DECLARE_PLAY_COMMAND
    void set_matrix(Gfx::FloatMatrix4x4 const&) override;
    Gfx::FloatMatrix4x4 canvas_matrix() const override;
    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    void push_clip(ReplayClip const&) override;
    void push_clip_path(Gfx::Path const&, Gfx::WindingRule) override;
    void push_layer(ReplayLayer const&) override;
    void push_mask(ReplayMask const&) override;
    void pop_mask(ReplayMask const&, Optional<DisplayListResourceId> mask_content) override;
    void pop() override;
    void push_device_space_plane_clip(Gfx::Path const&) override;

    void clip_path(Gfx::Path const&, Gfx::WindingRule, bool anti_aliased);

    SkPaint paint_style_to_skia_paint(DisplayListPaintStyle const&, Gfx::FloatRect const& bounding_rect);
    sk_sp<SkImageFilter> layer_image_filter(ReplayLayer const&);
    Gfx::Path path_from_data(DisplayListDataSpan) const;
    ReadonlySpan<Color> gradient_colors(DisplayListGradientColorStops) const;
    ReadonlySpan<float> gradient_positions(DisplayListGradientColorStops) const;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CompositedContextResolver const* m_composited_context_resolver { nullptr };

    // Layer filters are built from their bytes once per frame node and kept until the visual
    // context tree's structure changes, so a replayed frame does not rebuild them.
    struct LayerImageFilterCache;
    NonnullOwnPtr<LayerImageFilterCache> m_layer_image_filter_cache;
};

}
