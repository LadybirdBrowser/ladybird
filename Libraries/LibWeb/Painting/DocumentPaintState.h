/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

struct BlockingWheelEventRegionState {
    bool has_blocking_wheel_event_listeners { false };
    bool has_blocking_wheel_event_region_covering_viewport { false };
};

class WEB_API DocumentPaintState {
public:
    explicit DocumentPaintState(Layout::NodeArena&);

    void viewport_row_was_reset(DOM::Document&);

    BlockingWheelEventRegionState collect_root_blocking_wheel_event_regions(DOM::Document&);
    void build_stacking_context_tree_if_needed(DOM::Document&);
    void invalidate_stacking_context_tree();

    void refresh_scroll_state(DOM::Document&);
    void refresh_sticky_constraints(DOM::Document&);

    void assign_accumulated_visual_contexts(DOM::Document&);
    bool update_accumulated_visual_context_values(DOM::Document&, Layout::RustFFI::NodeSlotId);
    void update_visual_viewport_accumulated_visual_context(DOM::Document&);
    void set_visual_animations(DOM::Document&, Vector<Compositor::VisualAnimation>);
    bool visual_context_tree_needs_compositor_update() const { return m_visual_context_tree_needs_compositor_update; }
    void did_update_visual_context_tree_in_compositor() { m_visual_context_tree_needs_compositor_update = false; }
    void set_force_incompatible_visual_context_tree_rebuild_for_testing() { m_force_incompatible_visual_context_tree_rebuild_for_testing = true; }
    bool has_visual_context_tree() const;
    u64 accumulated_visual_context_tree_build_count() const { return m_accumulated_visual_context_tree_build_count; }

    void recompute_selection_states(DOM::Document&, DOM::Range&);
    void reset_selection_states(DOM::Document&);

    void invalidate_all_cached_paint(DOM::Document&);

    void set_needs_to_refresh_scroll_state(DOM::Document&, bool);

    ScrollStateSnapshot const& scroll_state_snapshot() const { return m_scroll_state_snapshot; }

    void set_boxes_with_auto_content_visibility(Vector<Layout::RustFFI::NodeSlotId> boxes) { m_boxes_with_auto_content_visibility = move(boxes); }
    Vector<Layout::RustFFI::NodeSlotId> const& boxes_with_auto_content_visibility() const { return m_boxes_with_auto_content_visibility; }

    AccumulatedVisualContextTree visual_context_tree(DOM::Document const&) const;
    u64 visual_context_tree_version(DOM::Document const&) const;

    void set_display_list_used_as_paint_command_cache_source(RefPtr<DisplayList const> display_list, DisplayListResourceSet referenced_resources)
    {
        m_display_list_used_as_paint_command_cache_source = move(display_list);
        m_paint_command_cache_source_referenced_resources = move(referenced_resources);
    }
    DisplayList const* display_list_used_as_paint_command_cache_source() const { return m_display_list_used_as_paint_command_cache_source.ptr(); }
    DisplayListResourceSet const& paint_command_cache_source_referenced_resources() const { return m_paint_command_cache_source_referenced_resources; }

    void append_paint_command_cache_source_resources(DisplayListResourceSet&) const;

private:
    void ensure_visual_context_tree(DOM::Document const&) const;
    AccumulatedVisualContextTree visual_context_tree_without_update(DOM::Document const&) const;
    void clear_scroll_state(DOM::Document&);

    NonnullRefPtr<Layout::NodeArena> m_layout_node_arena;
    bool m_stacking_context_tree_is_valid { false };

    ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_needs_to_refresh_scroll_state { true };

    Vector<Layout::RustFFI::NodeSlotId> m_boxes_with_auto_content_visibility;

    RefPtr<DisplayList const> m_display_list_used_as_paint_command_cache_source;
    DisplayListResourceSet m_paint_command_cache_source_referenced_resources;

    Vector<Compositor::VisualAnimation> m_visual_animations;
    RefPtr<VisualAnimationList const> m_visual_context_tree_visual_animations;
    u64 m_accumulated_visual_context_tree_build_count { 0 };
    bool m_visual_context_tree_needs_compositor_update { false };
    bool m_force_incompatible_visual_context_tree_rebuild_for_testing { false };
};

}
