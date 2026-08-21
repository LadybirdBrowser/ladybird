/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Painting {

class WEB_API ViewportPaintable final : public PaintableWithLines {
public:
    static NonnullRefPtr<ViewportPaintable> create(Layout::Viewport const&);
    virtual ~ViewportPaintable() override;

    virtual void reset_for_relayout() override;

    BlockingWheelEventRegionState collect_root_blocking_wheel_event_regions();
    void build_stacking_context_tree_if_needed();
    void invalidate_stacking_context_tree();

    void refresh_scroll_state();
    void refresh_sticky_constraints();
    CSSPixelPoint cumulative_scroll_offset_for_node(VisualContextIndex scroll_node_index) const;

    void assign_accumulated_visual_contexts();
    bool update_accumulated_visual_context_values(Paintable&);
    void update_visual_viewport_accumulated_visual_context();
    bool visual_context_tree_needs_compositor_update() const;
    void did_update_visual_context_tree_in_compositor();
    void set_force_incompatible_visual_context_tree_rebuild_for_testing();
    bool has_visual_context_tree() const;
    u64 accumulated_visual_context_tree_build_count() const;

    void recompute_selection_states(DOM::Range&);
    void reset_selection_states();

    // Throws away all cached paint output and schedules a repaint. For rare events that change how everything
    // renders, such as the window focus state changing.
    void invalidate_all_cached_paint();

    void set_needs_to_refresh_scroll_state(bool value);

    ScrollStateSnapshot const& scroll_state_snapshot() const;

    void set_paintable_boxes_with_auto_content_visibility(Vector<WeakPtr<Paintable>>);
    Vector<WeakPtr<Paintable>> const& paintable_boxes_with_auto_content_visibility() const;

    AccumulatedVisualContextTree const& visual_context_tree() const;
    AccumulatedVisualContextTree& visual_context_tree();

    void set_display_list_used_as_paint_command_cache_source(RefPtr<DisplayList const>, DisplayListResourceSet);
    DisplayList const* display_list_used_as_paint_command_cache_source() const;
    DisplayListResourceSet const& paint_command_cache_source_referenced_resources() const;

    // Cached command ranges keep pointing into the retained source until it rotates, so pruning the
    // backing resource storage must keep everything the source references alive.
    void append_paint_command_cache_source_resources(DisplayListResourceSet&) const;

private:
    explicit ViewportPaintable(Layout::Viewport const&);
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
