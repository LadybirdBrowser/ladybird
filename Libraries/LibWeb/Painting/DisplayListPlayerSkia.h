/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

class GrDirectContext;

namespace Web::Painting {

class DisplayListPlayerSkia final : public DisplayListPlayer {
public:
    DisplayListPlayerSkia(RefPtr<Gfx::SkiaBackendContext>);
    DisplayListPlayerSkia();

private:
    void flush() override;
    void draw_glyph_run(DrawGlyphRun const&) override;
    void fill_rect(FillRect const&) override;
    void draw_painting_surface(DrawPaintingSurface const&) override;
    void draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) override;
    void draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const&) override;
    void add_clip_rect(AddClipRect const&) override;
    void save(Save const&) override;
    void save_layer(SaveLayer const&) override;
    void restore(Restore const&) override;
    void translate(Translate const&) override;
    void push_stacking_context(PushStackingContext const&) override;
    void pop_stacking_context(PopStackingContext const&) override;
    void paint_linear_gradient(PaintLinearGradient const&) override;
    void paint_outer_box_shadow(PaintOuterBoxShadow const&) override;
    void paint_inner_box_shadow(PaintInnerBoxShadow const&) override;
    void paint_text_shadow(PaintTextShadow const&) override;
    void fill_rect_with_rounded_corners(FillRectWithRoundedCorners const&) override;
    void fill_path_using_color(FillPathUsingColor const&) override;
    void fill_path_using_paint_style(FillPathUsingPaintStyle const&) override;
    void stroke_path_using_color(StrokePathUsingColor const&) override;
    void stroke_path_using_paint_style(StrokePathUsingPaintStyle const&) override;
    void draw_ellipse(DrawEllipse const&) override;
    void fill_ellipse(FillEllipse const&) override;
    void draw_line(DrawLine const&) override;
    void apply_backdrop_filter(ApplyBackdropFilter const&) override;
    void draw_rect(DrawRect const&) override;
    void paint_radial_gradient(PaintRadialGradient const&) override;
    void paint_conic_gradient(PaintConicGradient const&) override;
    void draw_triangle_wave(DrawTriangleWave const&) override;
    void add_rounded_rect_clip(AddRoundedRectClip const&) override;
    void add_mask(AddMask const&) override;
    void paint_scrollbar(PaintScrollBar const&) override;
    void paint_nested_display_list(PaintNestedDisplayList const&) override;
    void apply_opacity(ApplyOpacity const&) override;
    void apply_composite_and_blending_operator(ApplyCompositeAndBlendingOperator const&) override;
    void apply_filters(ApplyFilters const&) override;
    void apply_transform(ApplyTransform const&) override;
    void apply_mask_bitmap(ApplyMaskBitmap const&) override;

    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    RefPtr<Gfx::SkiaBackendContext> m_context;
};

}
