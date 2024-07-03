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
    SkiaBackendContext() {};
    virtual ~SkiaBackendContext() {};

    virtual void flush_and_submit() {};
};

class DisplayListPlayerSkia : public DisplayListPlayer {
public:
    CommandResult draw_glyph_run(DrawGlyphRun const&) override;
    CommandResult fill_rect(FillRect const&) override;
    CommandResult draw_scaled_bitmap(DrawScaledBitmap const&) override;
    CommandResult draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) override;
    CommandResult add_clip_rect(AddClipRect const&) override;
    CommandResult save(Save const&) override;
    CommandResult restore(Restore const&) override;
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

    bool needs_prepare_glyphs_texture() const override { return false; }
    void prepare_glyph_texture(HashMap<Gfx::Font const*, HashTable<u32>> const&) override {};

    virtual void prepare_to_execute(size_t corner_clip_max_depth) override;

    bool needs_update_immutable_bitmap_texture_cache() const override { return false; }
    void update_immutable_bitmap_texture_cache(HashMap<u32, Gfx::ImmutableBitmap const*>&) override {};

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
    class SkiaSurface;
    SkiaSurface& surface() const;

    OwnPtr<SkiaSurface> m_surface;
    Function<void()> m_flush_context;
};

}
