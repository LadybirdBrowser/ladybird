/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/SegmentedVector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Painting/Command.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class DisplayList;

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList&);
    void set_surface(NonnullRefPtr<Gfx::PaintingSurface> surface) { m_surface = surface; }

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }

private:
    virtual void flush() = 0;
    virtual void draw_glyph_run(DrawGlyphRun const&) = 0;
    virtual void fill_rect(FillRect const&) = 0;
    virtual void draw_painting_surface(DrawPaintingSurface const&) = 0;
    virtual void draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) = 0;
    virtual void draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const&) = 0;
    virtual void save(Save const&) = 0;
    virtual void save_layer(SaveLayer const&) = 0;
    virtual void restore(Restore const&) = 0;
    virtual void translate(Translate const&) = 0;
    virtual void add_clip_rect(AddClipRect const&) = 0;
    virtual void push_stacking_context(PushStackingContext const&) = 0;
    virtual void pop_stacking_context(PopStackingContext const&) = 0;
    virtual void paint_linear_gradient(PaintLinearGradient const&) = 0;
    virtual void paint_radial_gradient(PaintRadialGradient const&) = 0;
    virtual void paint_conic_gradient(PaintConicGradient const&) = 0;
    virtual void paint_outer_box_shadow(PaintOuterBoxShadow const&) = 0;
    virtual void paint_inner_box_shadow(PaintInnerBoxShadow const&) = 0;
    virtual void paint_text_shadow(PaintTextShadow const&) = 0;
    virtual void fill_rect_with_rounded_corners(FillRectWithRoundedCorners const&) = 0;
    virtual void fill_path_using_color(FillPathUsingColor const&) = 0;
    virtual void fill_path_using_paint_style(FillPathUsingPaintStyle const&) = 0;
    virtual void stroke_path_using_color(StrokePathUsingColor const&) = 0;
    virtual void stroke_path_using_paint_style(StrokePathUsingPaintStyle const&) = 0;
    virtual void draw_ellipse(DrawEllipse const&) = 0;
    virtual void fill_ellipse(FillEllipse const&) = 0;
    virtual void draw_line(DrawLine const&) = 0;
    virtual void apply_backdrop_filter(ApplyBackdropFilter const&) = 0;
    virtual void draw_rect(DrawRect const&) = 0;
    virtual void draw_triangle_wave(DrawTriangleWave const&) = 0;
    virtual void add_rounded_rect_clip(AddRoundedRectClip const&) = 0;
    virtual void add_mask(AddMask const&) = 0;
    virtual void paint_nested_display_list(PaintNestedDisplayList const&) = 0;
    virtual void paint_scrollbar(PaintScrollBar const&) = 0;
    virtual void apply_opacity(ApplyOpacity const&) = 0;
    virtual void apply_composite_and_blending_operator(ApplyCompositeAndBlendingOperator const&) = 0;
    virtual void apply_filters(ApplyFilters const&) = 0;
    virtual void apply_transform(ApplyTransform const&) = 0;
    virtual void apply_mask_bitmap(ApplyMaskBitmap const&) = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    RefPtr<Gfx::PaintingSurface> m_surface;
};

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    static NonnullRefPtr<DisplayList> create()
    {
        return adopt_ref(*new DisplayList());
    }

    void append(Command&& command, Optional<i32> scroll_frame_id);

    struct CommandListItem {
        Optional<i32> scroll_frame_id;
        Command command;
    };

    AK::SegmentedVector<CommandListItem, 512> const& commands() const { return m_commands; }

    void set_scroll_state(ScrollState scroll_state) { m_scroll_state = move(scroll_state); }
    ScrollState const& scroll_state() const { return m_scroll_state; }

    void set_device_pixels_per_css_pixel(double device_pixels_per_css_pixel) { m_device_pixels_per_css_pixel = device_pixels_per_css_pixel; }
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }

private:
    DisplayList() = default;

    AK::SegmentedVector<CommandListItem, 512> m_commands;
    ScrollState m_scroll_state;
    double m_device_pixels_per_css_pixel;
};

}
