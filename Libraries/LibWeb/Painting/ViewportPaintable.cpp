/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

NonnullRefPtr<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return adopt_ref(*new ViewportPaintable(layout_viewport));
}

ViewportPaintable::ViewportPaintable(Layout::Viewport const& layout_viewport)
    : PaintableWithLines(layout_viewport)
{
    mirror_rust_reset_visual_context_state(document());
}

ViewportPaintable::~ViewportPaintable() = default;

AccumulatedVisualContextTree const& ViewportPaintable::visual_context_tree() const
{
    return document().paint_state().visual_context_tree(document());
}

AccumulatedVisualContextTree& ViewportPaintable::visual_context_tree()
{
    return document().paint_state().visual_context_tree(document());
}

BlockingWheelEventRegionState ViewportPaintable::collect_root_blocking_wheel_event_regions()
{
    return document().paint_state().collect_root_blocking_wheel_event_regions(document());
}

void ViewportPaintable::reset_for_relayout()
{
    PaintableWithLines::reset_for_relayout();
    document().paint_state().viewport_row_was_reset(document());
}

void ViewportPaintable::build_stacking_context_tree_if_needed()
{
    document().paint_state().build_stacking_context_tree_if_needed(document());
}

void ViewportPaintable::invalidate_stacking_context_tree()
{
    document().paint_state().invalidate_stacking_context_tree();
}

void ViewportPaintable::refresh_sticky_constraints()
{
    document().paint_state().refresh_sticky_constraints(document());
}

void ViewportPaintable::set_needs_to_refresh_scroll_state(bool value)
{
    document().paint_state().set_needs_to_refresh_scroll_state(document(), value);
}

CSSPixelPoint ViewportPaintable::cumulative_scroll_offset_for_node(VisualContextIndex scroll_node_index) const
{
    return document().paint_state().cumulative_scroll_offset_for_node(document(), scroll_node_index);
}

void ViewportPaintable::assign_accumulated_visual_contexts()
{
    document().paint_state().assign_accumulated_visual_contexts(document());
}

bool ViewportPaintable::update_accumulated_visual_context_values(Paintable& paintable_box)
{
    return document().paint_state().update_accumulated_visual_context_values(document(), paintable_box);
}

void ViewportPaintable::update_visual_viewport_accumulated_visual_context()
{
    document().paint_state().update_visual_viewport_accumulated_visual_context(document());
}

void ViewportPaintable::append_paint_command_cache_source_resources(DisplayListResourceSet& retained_resources) const
{
    document().paint_state().append_paint_command_cache_source_resources(retained_resources);
}

void ViewportPaintable::invalidate_all_cached_paint()
{
    document().paint_state().invalidate_all_cached_paint(document());
}

void ViewportPaintable::refresh_scroll_state()
{
    document().paint_state().refresh_scroll_state(document());
}

void ViewportPaintable::reset_selection_states()
{
    document().paint_state().reset_selection_states(document());
}

void ViewportPaintable::recompute_selection_states(DOM::Range& range)
{
    document().paint_state().recompute_selection_states(document(), range);
}

bool ViewportPaintable::visual_context_tree_needs_compositor_update() const
{
    return document().paint_state().visual_context_tree_needs_compositor_update();
}

void ViewportPaintable::did_update_visual_context_tree_in_compositor()
{
    document().paint_state().did_update_visual_context_tree_in_compositor();
}

void ViewportPaintable::set_force_incompatible_visual_context_tree_rebuild_for_testing()
{
    document().paint_state().set_force_incompatible_visual_context_tree_rebuild_for_testing();
}

bool ViewportPaintable::has_visual_context_tree() const
{
    return document().paint_state().has_visual_context_tree();
}

u64 ViewportPaintable::accumulated_visual_context_tree_build_count() const
{
    return document().paint_state().accumulated_visual_context_tree_build_count();
}

ScrollStateSnapshot const& ViewportPaintable::scroll_state_snapshot() const
{
    return document().paint_state().scroll_state_snapshot();
}

void ViewportPaintable::set_paintable_boxes_with_auto_content_visibility(Vector<WeakPtr<Paintable>> paintable_boxes)
{
    document().paint_state().set_paintable_boxes_with_auto_content_visibility(move(paintable_boxes));
}

Vector<WeakPtr<Paintable>> const& ViewportPaintable::paintable_boxes_with_auto_content_visibility() const
{
    return document().paint_state().paintable_boxes_with_auto_content_visibility();
}

void ViewportPaintable::set_display_list_used_as_paint_command_cache_source(RefPtr<DisplayList const> display_list, DisplayListResourceSet referenced_resources)
{
    document().paint_state().set_display_list_used_as_paint_command_cache_source(move(display_list), move(referenced_resources));
}

DisplayList const* ViewportPaintable::display_list_used_as_paint_command_cache_source() const
{
    return document().paint_state().display_list_used_as_paint_command_cache_source();
}

DisplayListResourceSet const& ViewportPaintable::paint_command_cache_source_referenced_resources() const
{
    return document().paint_state().paint_command_cache_source_referenced_resources();
}

}
