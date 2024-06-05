/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Painter.h>
#include <LibGfx/Quad.h>
#include <LibWeb/Painting/RecordingPainter.h>

namespace Web::Painting {

class AffineCommandExecutorCPU : public CommandExecutor {
public:
    CommandResult draw_glyph_run(DrawGlyphRun const&) override;
    CommandResult draw_text(DrawText const&) override;
    CommandResult fill_rect(FillRect const&) override;
    CommandResult draw_scaled_bitmap(DrawScaledBitmap const&) override;
    CommandResult draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) override;
    CommandResult set_clip_rect(SetClipRect const&) override;
    CommandResult clear_clip_rect(ClearClipRect const&) override;
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
    CommandResult draw_signed_distance_field(DrawSignedDistanceField const&) override;
    CommandResult apply_backdrop_filter(ApplyBackdropFilter const&) override;
    CommandResult draw_rect(DrawRect const&) override;
    CommandResult paint_radial_gradient(PaintRadialGradient const&) override;
    CommandResult paint_conic_gradient(PaintConicGradient const&) override;
    CommandResult draw_triangle_wave(DrawTriangleWave const&) override;
    CommandResult sample_under_corners(SampleUnderCorners const&) override;
    CommandResult blit_corner_clipping(BlitCornerClipping const&) override;

    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    bool needs_prepare_glyphs_texture() const override { return false; }
    void prepare_glyph_texture(HashMap<Gfx::Font const*, HashTable<u32>> const&) override {};

    virtual void prepare_to_execute(size_t) override { }

    bool needs_update_immutable_bitmap_texture_cache() const override { return false; }
    void update_immutable_bitmap_texture_cache(HashMap<u32, Gfx::ImmutableBitmap const*>&) override {};

    AffineCommandExecutorCPU(Gfx::Bitmap& bitmap, Gfx::AffineTransform transform, Gfx::IntRect clip);

    virtual ~AffineCommandExecutorCPU() override = default;

private:
    // FIXME: Support masking.
    // FIXME: Support opacity < 1.0f.
    struct StackingContext {
        Gfx::AffineTransform transform;
        Gfx::FloatQuad clip;
        Gfx::FloatRect clip_bounds;
    };

    Gfx::AntiAliasingPainter aa_painter()
    {
        return Gfx::AntiAliasingPainter(m_painter);
    }

    StackingContext& stacking_context()
    {
        return m_stacking_contexts.last();
    }

    StackingContext const& stacking_context() const
    {
        return m_stacking_contexts.last();
    }

    Gfx::Painter m_painter;
    Vector<StackingContext> m_stacking_contexts;
};

}
