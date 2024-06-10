/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MaybeOwned.h>
#include <LibAccelGfx/Painter.h>
#include <LibWeb/Painting/RecordingPainter.h>

namespace Web::Painting {

class CommandExecutorGPU : public CommandExecutor {
public:
    CommandResult draw_glyph_run(DrawGlyphRun const&) override;
    CommandResult draw_text(DrawText const&) override;
    CommandResult fill_rect(FillRect const&) override;
    CommandResult draw_scaled_bitmap(DrawScaledBitmap const&) override;
    CommandResult draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) override;
    CommandResult save(Save const&) override;
    CommandResult restore(Restore const&) override;
    CommandResult add_clip_rect(AddClipRect const&) override;
    CommandResult push_stacking_context(PushStackingContext const&) override;
    CommandResult pop_stacking_context(PopStackingContext const&) override;
    CommandResult paint_linear_gradient(PaintLinearGradient const&) override;
    CommandResult paint_outer_box_shadow(PaintOuterBoxShadow const&) override;
    CommandResult paint_inner_box_shadow(PaintInnerBoxShadow const&) override;
    CommandResult paint_text_shadow(PaintTextShadow const&) override;
    CommandResult fill_rect_with_rounded_corners(FillRectWithRoundedCorners const&) override;
    CommandResult fill_path_using_color(FillPathUsingColor const&) override;
    CommandResult fill_path_using_paint_style(FillPathUsingPaintStyle const&) override;
    CommandResult stroke_path_using_color(StrokePathUsingColor const&) override;
    CommandResult stroke_path_using_paint_style(StrokePathUsingPaintStyle const&) override;
    CommandResult draw_ellipse(DrawEllipse const&) override;
    CommandResult fill_ellipse(FillEllipse const&) override;
    CommandResult draw_line(DrawLine const&) override;
    CommandResult apply_backdrop_filter(ApplyBackdropFilter const&) override;
    CommandResult draw_rect(DrawRect const&) override;
    CommandResult paint_radial_gradient(PaintRadialGradient const&) override;
    CommandResult paint_conic_gradient(PaintConicGradient const&) override;
    CommandResult draw_triangle_wave(DrawTriangleWave const&) override;
    CommandResult sample_under_corners(SampleUnderCorners const&) override;
    CommandResult blit_corner_clipping(BlitCornerClipping const&) override;

    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    virtual bool needs_prepare_glyphs_texture() const override { return true; }
    void prepare_glyph_texture(HashMap<Gfx::Font const*, HashTable<u32>> const&) override;

    virtual void prepare_to_execute(size_t corner_clip_max_depth) override;

    bool needs_update_immutable_bitmap_texture_cache() const override { return true; }
    void update_immutable_bitmap_texture_cache(HashMap<u32, Gfx::ImmutableBitmap const*>&) override;

    CommandExecutorGPU(AccelGfx::Context&, Gfx::Bitmap& bitmap);
    ~CommandExecutorGPU() override;

private:
    Gfx::Bitmap& m_target_bitmap;
    AccelGfx::Context& m_context;

    struct StackingContext {
        RefPtr<AccelGfx::Canvas> canvas;
        MaybeOwned<AccelGfx::Painter> painter;
        float opacity;
        Gfx::IntRect destination;
        Gfx::AffineTransform transform;
        int stacking_context_depth { 0 };
    };

    struct BorderRadiusCornerClipper {
        RefPtr<AccelGfx::Canvas> corners_sample_canvas;

        Gfx::FloatRect page_top_left_rect;
        Gfx::FloatRect page_top_right_rect;
        Gfx::FloatRect page_bottom_right_rect;
        Gfx::FloatRect page_bottom_left_rect;

        Gfx::FloatRect sample_canvas_top_left_rect;
        Gfx::FloatRect sample_canvas_top_right_rect;
        Gfx::FloatRect sample_canvas_bottom_right_rect;
        Gfx::FloatRect sample_canvas_bottom_left_rect;
    };

    [[nodiscard]] AccelGfx::Painter const& painter() const { return *m_stacking_contexts.last().painter; }
    [[nodiscard]] AccelGfx::Painter& painter() { return *m_stacking_contexts.last().painter; }

    Vector<StackingContext> m_stacking_contexts;
    Vector<OwnPtr<BorderRadiusCornerClipper>> m_corner_clippers;
};

}
