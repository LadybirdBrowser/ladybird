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
    GC_CELL(ViewportPaintable, PaintableWithLines);
    GC_DECLARE_ALLOCATOR(ViewportPaintable);

public:
    static GC::Ref<ViewportPaintable> create(Layout::Viewport const&);
    virtual ~ViewportPaintable() override;

    virtual void reset_for_relayout() override;

    void paint_all_phases(DisplayListRecordingContext&);
    void build_stacking_context_tree_if_needed();

    void assign_scroll_frames();
    void refresh_scroll_state();

    void assign_accumulated_visual_contexts();

    void resolve_paint_only_properties();

    GC::Ptr<Selection::Selection> selection() const;
    void recompute_selection_states(DOM::Range&);

    bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y) override;

    void set_needs_to_refresh_scroll_state(bool value) { m_needs_to_refresh_scroll_state = value; }

    ScrollState const& scroll_state() const { return m_scroll_state; }
    ScrollStateSnapshot const& scroll_state_snapshot() const { return m_scroll_state_snapshot; }

    void set_paintable_boxes_with_auto_content_visibility(Vector<GC::Ref<PaintableBox>> paintable_boxes) { m_paintable_boxes_with_auto_content_visibility = move(paintable_boxes); }
    ReadonlySpan<GC::Ref<PaintableBox>> paintable_boxes_with_auto_content_visibility() const { return m_paintable_boxes_with_auto_content_visibility; }

    size_t allocate_accumulated_visual_context_id() { return m_next_accumulated_visual_context_id++; }

private:
    virtual bool is_viewport_paintable() const override { return true; }

    void build_stacking_context_tree();

    explicit ViewportPaintable(Layout::Viewport const&);

    virtual void visit_edges(Visitor&) override;

    ScrollState m_scroll_state;
    ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_needs_to_refresh_scroll_state { true };

    Vector<GC::Ref<PaintableBox>> m_paintable_boxes_with_auto_content_visibility;

    size_t m_next_accumulated_visual_context_id { 1 };

    RefPtr<AccumulatedVisualContext const> m_visual_viewport_context;
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
