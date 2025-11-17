/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ClipFrame.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList&, ScrollStateSnapshotByDisplayList&&, RefPtr<Gfx::PaintingSurface>);

protected:
    Gfx::PaintingSurface& surface() const { return m_surfaces.last(); }
    void execute_impl(DisplayList&, ScrollStateSnapshot const& scroll_state, RefPtr<Gfx::PaintingSurface>);

    ScrollStateSnapshotByDisplayList m_scroll_state_snapshots_by_display_list;

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
    virtual void fill_path(FillPath const&) = 0;
    virtual void stroke_path(StrokePath const&) = 0;
    virtual void draw_ellipse(DrawEllipse const&) = 0;
    virtual void fill_ellipse(FillEllipse const&) = 0;
    virtual void draw_line(DrawLine const&) = 0;
    virtual void apply_backdrop_filter(ApplyBackdropFilter const&) = 0;
    virtual void draw_rect(DrawRect const&) = 0;
    virtual void add_rounded_rect_clip(AddRoundedRectClip const&) = 0;
    virtual void add_mask(AddMask const&) = 0;
    virtual void paint_nested_display_list(PaintNestedDisplayList const&) = 0;
    virtual void paint_scrollbar(PaintScrollBar const&) = 0;
    virtual void apply_opacity(ApplyOpacity const&) = 0;
    virtual void apply_composite_and_blending_operator(ApplyCompositeAndBlendingOperator const&) = 0;
    virtual void apply_filter(ApplyFilter const&) = 0;
    virtual void apply_transform(ApplyTransform const&) = 0;
    virtual void apply_mask_bitmap(ApplyMaskBitmap const&) = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    void apply_clip_frame(ClipFrame const&, ScrollStateSnapshot const&, DevicePixelConverter const&);
    void remove_clip_frame(ClipFrame const&);

    Vector<NonnullRefPtr<Gfx::PaintingSurface>, 1> m_surfaces;
};

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    static NonnullRefPtr<DisplayList> create(double device_pixels_per_css_pixel)
    {
        return adopt_ref(*new DisplayList(device_pixels_per_css_pixel));
    }

    void append(DisplayListCommand&& command, Optional<i32> scroll_frame_id, RefPtr<ClipFrame const>);

    struct DisplayListCommandWithScrollAndClip {
        Optional<i32> scroll_frame_id;
        RefPtr<ClipFrame const> clip_frame;
        DisplayListCommand command;
    };

    auto& commands(Badge<DisplayListRecorder>) { return m_commands; }
    auto const& commands() const { return m_commands; }
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }

    String dump() const;

    template<typename Callback>
    void for_each_command_in_range(size_t start, size_t end, Callback callback)
    {
        for (auto index = start; index < end; ++index) {
            if (callback(m_commands[index].command, m_commands[index].scroll_frame_id) == IterationDecision::Break)
                break;
        }
    }

    static constexpr size_t VISUAL_VIEWPORT_TRANSFORM_INDEX = 1;
    void set_visual_viewport_transform(Gfx::FloatMatrix4x4 t) { m_commands[VISUAL_VIEWPORT_TRANSFORM_INDEX].command.get<ApplyTransform>().matrix = t; }

private:
    DisplayList(double device_pixels_per_css_pixel)
        : m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
    {
    }

    AK::SegmentedVector<DisplayListCommandWithScrollAndClip, 512> m_commands;
    double m_device_pixels_per_css_pixel;
    Optional<Gfx::FloatMatrix4x4> m_visual_viewport_transform;
};

}
