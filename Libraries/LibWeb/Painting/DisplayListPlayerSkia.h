/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

class GrDirectContext;
class SkPaint;

namespace Web::Painting {

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
    void play_command(ApplyEffects const&, Gfx::Filter const*) override;
    void apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 const&) override;

    void add_clip_path(Gfx::Path const&, Gfx::WindingRule) override;

    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    SkPaint paint_style_to_skia_paint(DisplayListPaintStyle const&, Gfx::FloatRect const& bounding_rect);
    Gfx::Path path_from_data(DisplayListDataSpan) const;
    ReadonlySpan<Color> gradient_colors(DisplayListGradientColorStops) const;
    ReadonlySpan<float> gradient_positions(DisplayListGradientColorStops) const;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CompositedContextResolver const* m_composited_context_resolver { nullptr };
};

}
