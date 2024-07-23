/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/SegmentedVector.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/Command.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>

namespace Web::Painting {

class DisplayList;

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList& display_list);

private:
    virtual void draw_glyph_run(DrawGlyphRun const&) = 0;
    virtual void fill_rect(FillRect const&) = 0;
    virtual void draw_scaled_bitmap(DrawScaledBitmap const&) = 0;
    virtual void draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) = 0;
    virtual void draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const&) = 0;
    virtual void save(Save const&) = 0;
    virtual void restore(Restore const&) = 0;
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
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;
};

class DisplayList : public RefCounted<DisplayList> {
public:
    static NonnullRefPtr<DisplayList> create()
    {
        return adopt_ref(*new DisplayList());
    }

    void append(Command&& command, Optional<i32> scroll_frame_id);

    void apply_scroll_offsets(Vector<Gfx::IntPoint> const& offsets_by_frame_id);

    bool is_empty() const { return m_commands.is_empty(); }

    struct CommandListItem {
        Optional<i32> scroll_frame_id;
        Command command;
    };

    AK::SegmentedVector<CommandListItem, 512> const& commands() const { return m_commands; }

private:
    DisplayList() = default;

    AK::SegmentedVector<CommandListItem, 512> m_commands;
};

}
