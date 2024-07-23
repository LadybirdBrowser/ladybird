/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#    include <LibCore/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibCore/VulkanContext.h>
#endif

namespace Web::Painting {

class SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
    SkiaBackendContext() { }
    virtual ~SkiaBackendContext() { }

    virtual void flush_and_submit() { }
};

class DisplayListPlayerSkia : public DisplayListPlayer {
public:
    DisplayListPlayerSkia(Gfx::Bitmap&);

#ifdef USE_VULKAN
    static OwnPtr<SkiaBackendContext> create_vulkan_context(Core::VulkanContext&);
    DisplayListPlayerSkia(SkiaBackendContext&, Gfx::Bitmap&);
#endif

#ifdef AK_OS_MACOS
    static OwnPtr<SkiaBackendContext> create_metal_context(Core::MetalContext const&);
    DisplayListPlayerSkia(SkiaBackendContext&, Core::MetalTexture&);
#endif

    virtual ~DisplayListPlayerSkia() override;

private:
    void draw_glyph_run(DrawGlyphRun const&) override;
    void fill_rect(FillRect const&) override;
    void draw_scaled_bitmap(DrawScaledBitmap const&) override;
    void draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) override;
    void draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const&) override;
    void add_clip_rect(AddClipRect const&) override;
    void save(Save const&) override;
    void restore(Restore const&) override;
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

    bool would_be_fully_clipped_by_painter(Gfx::IntRect) const override;

    void apply_mask_painted_from(DisplayList&, Gfx::IntRect, bool translate = true);

    class SkiaSurface;
    SkiaSurface& surface() const;

    OwnPtr<SkiaSurface> m_surface;
    Function<void()> m_flush_context;
};

}
