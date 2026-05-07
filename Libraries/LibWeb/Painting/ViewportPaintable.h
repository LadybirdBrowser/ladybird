/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    void build_stacking_context_tree_if_needed();

    void assign_scroll_frames();
    void refresh_scroll_state();

    void assign_accumulated_visual_contexts();

    GC::Ptr<Selection::Selection> selection() const;
    void recompute_selection_states(DOM::Range&);

    bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y) override;

    void set_needs_to_refresh_scroll_state(bool value) { m_needs_to_refresh_scroll_state = value; }

    ScrollState const& scroll_state() const { return m_scroll_state; }
    ScrollStateSnapshot const& scroll_state_snapshot() const { return m_scroll_state_snapshot; }

    void set_paintable_boxes_with_auto_content_visibility(Vector<WeakPtr<PaintableBox>> paintable_boxes) { m_paintable_boxes_with_auto_content_visibility = move(paintable_boxes); }
    Vector<WeakPtr<PaintableBox>> const& paintable_boxes_with_auto_content_visibility() const { return m_paintable_boxes_with_auto_content_visibility; }

    AccumulatedVisualContextTree const& visual_context_tree() const
    {
        VERIFY(m_visual_context_tree);
        return *m_visual_context_tree;
    }
    AccumulatedVisualContextTree& visual_context_tree()
    {
        VERIFY(m_visual_context_tree);
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

    RefPtr<AccumulatedVisualContextTree> m_visual_context_tree;
    VisualContextIndex m_visual_viewport_context_index {};
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
