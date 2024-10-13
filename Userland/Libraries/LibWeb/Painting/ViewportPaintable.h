/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class ViewportPaintable final : public PaintableWithLines {
    JS_CELL(ViewportPaintable, PaintableWithLines);
    JS_DECLARE_ALLOCATOR(ViewportPaintable);

public:
    static JS::NonnullGCPtr<ViewportPaintable> create(Layout::Viewport const&);
    virtual ~ViewportPaintable() override;

    void paint_all_phases(PaintContext&);
    void build_stacking_context_tree_if_needed();

    void assign_scroll_frames();
    void refresh_scroll_state();

    HashMap<JS::GCPtr<PaintableBox const>, RefPtr<ClipFrame>> clip_state;
    void assign_clip_frames();

    void resolve_paint_only_properties();

    JS::GCPtr<Selection::Selection> selection() const;
    void recompute_selection_states(DOM::Range&);
    void update_selection();

    bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y) override;

    void set_needs_to_refresh_scroll_state(bool value) { m_needs_to_refresh_scroll_state = value; }

    ScrollState const& scroll_state() const { return m_scroll_state; }

private:
    void build_stacking_context_tree();

    explicit ViewportPaintable(Layout::Viewport const&);

    virtual void visit_edges(Visitor&) override;

    ScrollState m_scroll_state;
    bool m_needs_to_refresh_scroll_state { true };
};

}
