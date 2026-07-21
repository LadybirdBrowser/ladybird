/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
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

    void register_scroll_node(AccumulatedVisualContextTree& visual_context_tree_being_built, VisualContextIndex node_index, Paintable const&, VisualContextIndex parent_index);
    void register_sticky_node(AccumulatedVisualContextTree& visual_context_tree_being_built, VisualContextIndex node_index, Paintable const&, VisualContextIndex parent_index);
    void refresh_scroll_state();
    void refresh_sticky_constraints();
    CSSPixelPoint cumulative_scroll_offset_for_node(VisualContextIndex scroll_node_index) const;

    void assign_accumulated_visual_contexts();
    bool update_accumulated_visual_context_values(Paintable&);
    void update_visual_viewport_accumulated_visual_context();
    bool visual_context_tree_needs_compositor_update() const { return m_visual_context_tree_needs_compositor_update; }
    void did_update_visual_context_tree_in_compositor() { m_visual_context_tree_needs_compositor_update = false; }
    void set_force_incompatible_visual_context_tree_rebuild_for_testing() { m_force_incompatible_visual_context_tree_rebuild_for_testing = true; }
    bool has_visual_context_tree() const { return m_visual_context_tree.has_value(); }
    u64 accumulated_visual_context_tree_build_count() const { return m_accumulated_visual_context_tree_build_count; }

    GC::Ptr<Selection::Selection> selection() const;
    void recompute_selection_states(DOM::Range&);
    void reset_selection_states();

    // Throws away all cached paint output and schedules a repaint. For rare events that change how everything
    // renders, such as the window focus state changing.
    void invalidate_all_cached_paint();

    bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double wheel_delta_x, double wheel_delta_y) override;

    void set_needs_to_refresh_scroll_state(bool value) { m_needs_to_refresh_scroll_state = value; }

    ScrollState const& scroll_state() const { return m_scroll_state; }
    ScrollStateSnapshot const& scroll_state_snapshot() const { return m_scroll_state_snapshot; }

    void set_paintable_boxes_with_auto_content_visibility(Vector<WeakPtr<Paintable>> paintable_boxes) { m_paintable_boxes_with_auto_content_visibility = move(paintable_boxes); }
    Vector<WeakPtr<Paintable>> const& paintable_boxes_with_auto_content_visibility() const { return m_paintable_boxes_with_auto_content_visibility; }

    AccumulatedVisualContextTree const& visual_context_tree() const;
    AccumulatedVisualContextTree& visual_context_tree();

    void set_display_list_used_as_paint_command_cache_source(RefPtr<DisplayList const> display_list, DisplayListResourceSet referenced_resources)
    {
        m_display_list_used_as_paint_command_cache_source = move(display_list);
        m_paint_command_cache_source_referenced_resources = move(referenced_resources);
    }
    DisplayList const* display_list_used_as_paint_command_cache_source() const { return m_display_list_used_as_paint_command_cache_source.ptr(); }
    DisplayListResourceSet const& paint_command_cache_source_referenced_resources() const { return m_paint_command_cache_source_referenced_resources; }

    // Cached command ranges keep pointing into the retained source until it rotates, so pruning the
    // backing resource storage must keep everything the source references alive.
    void append_paint_command_cache_source_resources(DisplayListResourceSet&) const;

private:
    friend void update_visual_viewport_accumulated_visual_context(ViewportPaintable&);

    virtual bool is_viewport_paintable() const override { return true; }

    void ensure_visual_context_tree() const;
    void build_stacking_context_tree();
    void clear_scroll_state();
    void precompute_sticky_constraints(ScrollStateSlot, Paintable const&);

    explicit ViewportPaintable(Layout::Viewport const&);

    ScrollState m_scroll_state;
    ScrollStateSnapshot m_scroll_state_snapshot;
    bool m_needs_to_refresh_scroll_state { true };

    Vector<WeakPtr<Paintable>> m_paintable_boxes_with_auto_content_visibility;

    RefPtr<DisplayList const> m_display_list_used_as_paint_command_cache_source;
    DisplayListResourceSet m_paint_command_cache_source_referenced_resources;

    Optional<AccumulatedVisualContextTree> m_visual_context_tree;
    u64 m_accumulated_visual_context_tree_build_count { 0 };
    bool m_visual_context_tree_needs_compositor_update { false };
    bool m_force_incompatible_visual_context_tree_rebuild_for_testing { false };
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
