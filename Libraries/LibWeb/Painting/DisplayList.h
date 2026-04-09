/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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
#include <LibGfx/PaintStyle.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class DisplayListPlayer {
public:
    virtual ~DisplayListPlayer() = default;

    void execute(DisplayList&, ScrollStateSnapshotByDisplayList&&, RefPtr<Gfx::PaintingSurface>);

protected:
    Gfx::PaintingSurface& surface() const { return *m_surface; }
    void execute_impl(DisplayList&, ScrollStateSnapshot const& scroll_state);
    void execute_display_list_into_surface(DisplayList&, Gfx::PaintingSurface&);

    ScrollStateSnapshotByDisplayList m_scroll_state_snapshots_by_display_list;

private:
    virtual void flush() = 0;
    virtual void draw_glyph_run(DrawGlyphRun const&) = 0;
    virtual void fill_rect(FillRect const&) = 0;
    virtual void draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const&) = 0;
    virtual void draw_repeated_immutable_bitmap(DrawRepeatedImmutableBitmap const&) = 0;
    virtual void draw_external_content(DrawExternalContent const&) = 0;
    virtual void save(Save const&) = 0;
    virtual void save_layer(SaveLayer const&) = 0;
    virtual void restore(Restore const&) = 0;
    virtual void translate(Translate const&) = 0;
    virtual void add_clip_rect(AddClipRect const&) = 0;
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
    virtual void paint_nested_display_list(PaintNestedDisplayList const&) = 0;
    virtual void paint_scrollbar(PaintScrollBar const&) = 0;
    virtual void apply_effects(ApplyEffects const&) = 0;
    virtual void apply_transform(Gfx::FloatPoint origin, Gfx::FloatMatrix4x4 const&) = 0;
    virtual bool would_be_fully_clipped_by_painter(Gfx::IntRect) const = 0;

    virtual void add_clip_path(Gfx::Path const&) = 0;

    RefPtr<Gfx::PaintingSurface> m_surface;
};

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    static NonnullRefPtr<DisplayList> create()
    {
        return adopt_ref(*new DisplayList());
    }

    void append(DisplayListCommand&& command, RefPtr<AccumulatedVisualContext const> context);

    struct CommandListItem {
        RefPtr<AccumulatedVisualContext const> context;
        DisplayListCommand command;
    };

    auto& commands(Badge<DisplayListRecorder>) { return m_commands; }
    auto const& commands() const { return m_commands; }

private:
    DisplayList() = default;

    AK::SegmentedVector<CommandListItem, 512> m_commands;
};

}
