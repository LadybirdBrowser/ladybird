/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class WEB_API ViewportPaintable final : public PaintableWithLines {
public:
    static NonnullRefPtr<ViewportPaintable> create(Layout::Viewport const&);
    virtual ~ViewportPaintable() override;
    virtual StringView class_name() const override { return "ViewportPaintable"sv; }

    virtual void reset_for_relayout() override;

    void paint_all_phases(DisplayListRecordingContext&);
    void initialize_async_scrolling_metadata_recording(DisplayListRecordingContext&);
    void finalize_async_scrolling_metadata_recording(DisplayListRecordingContext&, HTML::LocalNavigable&, Gfx::IntRect viewport_rect);
    void build_stacking_context_tree_if_needed();

    void assign_scroll_frames();
    void refresh_scroll_state();

    void assign_accumulated_visual_contexts();
    void update_visual_viewport_accumulated_visual_context();
    bool visual_context_tree_needs_compositor_update() const { return m_visual_context_tree_needs_compositor_update; }
    void did_update_visual_context_tree_in_compositor() { m_visual_context_tree_needs_compositor_update = false; }
    void set_force_incompatible_visual_context_tree_rebuild_for_testing() { m_force_incompatible_visual_context_tree_rebuild_for_testing = true; }
    bool has_visual_context_tree() const { return m_visual_context_tree.has_value(); }

    GC::Ptr<Selection::Selection> selection() const;
    void recompute_selection_states(DOM::Range&);
    void reset_selection_states();

    bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double wheel_delta_x, double wheel_delta_y) override;

    void set_needs_to_refresh_scroll_state(bool value) { m_needs_to_refresh_scroll_state = value; }

    ScrollState const& scroll_state() const { return m_scroll_state; }
    ScrollStateSnapshot const& scroll_state_snapshot() const { return m_scroll_state_snapshot; }

    void set_paintable_boxes_with_auto_content_visibility(Vector<WeakPtr<PaintableBox>> paintable_boxes) { m_paintable_boxes_with_auto_content_visibility = move(paintable_boxes); }
    Vector<WeakPtr<PaintableBox>> const& paintable_boxes_with_auto_content_visibility() const { return m_paintable_boxes_with_auto_content_visibility; }

    AccumulatedVisualContextTree const& visual_context_tree() const
    {
        VERIFY(m_visual_context_tree.has_value());
        return *m_visual_context_tree;
    }
    AccumulatedVisualContextTree& visual_context_tree()
    {
        VERIFY(m_visual_context_tree.has_value());
        return *m_visual_context_tree;
    }

private:
    virtual bool is_viewport_paintable() const override { return true; }

    void build_stacking_context_tree();

    explicit ViewportPaintable(Layout::Viewport const&);

    ScrollState m_scroll_state;
    ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_needs_to_refresh_scroll_state { true };

    Vector<WeakPtr<PaintableBox>> m_paintable_boxes_with_auto_content_visibility;

    Optional<AccumulatedVisualContextTree> m_visual_context_tree;
    bool m_visual_context_tree_needs_compositor_update { false };
    bool m_force_incompatible_visual_context_tree_rebuild_for_testing { false };
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
