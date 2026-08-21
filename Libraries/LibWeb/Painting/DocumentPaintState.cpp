/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

DocumentPaintState::DocumentPaintState(Layout::NodeArena& layout_node_arena)
    : m_layout_node_arena(layout_node_arena)
{
}

void DocumentPaintState::ensure_visual_context_tree(DOM::Document const& document) const
{
    const_cast<DOM::Document&>(document).update_paint_and_hit_testing_properties_if_needed();
}

AccumulatedVisualContextTree const& DocumentPaintState::visual_context_tree(DOM::Document const& document) const
{
    ensure_visual_context_tree(document);
    VERIFY(m_visual_context_tree.has_value());
    return *m_visual_context_tree;
}

AccumulatedVisualContextTree& DocumentPaintState::visual_context_tree(DOM::Document& document)
{
    ensure_visual_context_tree(document);
    VERIFY(m_visual_context_tree.has_value());
    return *m_visual_context_tree;
}

BlockingWheelEventRegionState DocumentPaintState::collect_root_blocking_wheel_event_regions(DOM::Document& document)
{
    GC::Ptr<DOM::EventTarget> roots[] = {
        document.navigable() ? document.navigable()->active_window() : nullptr,
        &document,
        document.document_element(),
        document.body(),
    };
    for (auto target : roots) {
        if (target && target->has_blocking_wheel_event_listener()) {
            return {
                .has_blocking_wheel_event_listeners = true,
                .has_blocking_wheel_event_region_covering_viewport = true,
            };
        }
    }
    return {};
}

void DocumentPaintState::viewport_row_was_reset(ViewportPaintable& viewport_paintable)
{
    clear_scroll_state(viewport_paintable);
    m_paintable_boxes_with_auto_content_visibility.clear();
    m_visual_context_tree_needs_compositor_update = false;
}

void DocumentPaintState::build_stacking_context_tree_if_needed(ViewportPaintable& viewport_paintable)
{
    if (m_stacking_context_tree_is_valid)
        return;
    rust_build_stacking_context_tree(viewport_paintable);
    m_stacking_context_tree_is_valid = true;
}

void DocumentPaintState::invalidate_stacking_context_tree()
{
    m_stacking_context_tree_is_valid = false;
}

void DocumentPaintState::refresh_sticky_constraints(ViewportPaintable& viewport_paintable)
{
    m_needs_to_refresh_scroll_state = true;
    mirror_rust_refresh_sticky_constraints(viewport_paintable);
}

void DocumentPaintState::set_needs_to_refresh_scroll_state(ViewportPaintable& viewport_paintable, bool value)
{
    m_needs_to_refresh_scroll_state = value;
    mirror_rust_set_needs_to_refresh_scroll_state(viewport_paintable, value);
}

void DocumentPaintState::clear_scroll_state(ViewportPaintable& viewport_paintable)
{
    m_scroll_state_snapshot = {};
    m_needs_to_refresh_scroll_state = true;
    mirror_rust_clear_scroll_state(viewport_paintable);
}

CSSPixelPoint DocumentPaintState::cumulative_scroll_offset_for_node(ViewportPaintable const& viewport_paintable, VisualContextIndex scroll_node_index) const
{
    return rust_cumulative_scroll_offset_for_node(viewport_paintable, scroll_node_index);
}

void DocumentPaintState::assign_accumulated_visual_contexts(ViewportPaintable& viewport_paintable)
{
    clear_scroll_state(viewport_paintable);
    ++m_accumulated_visual_context_tree_build_count;
    auto forced_incompatible_rebuild = m_force_incompatible_visual_context_tree_rebuild_for_testing;
    m_force_incompatible_visual_context_tree_rebuild_for_testing = false;
    auto is_compatible = rust_assign_accumulated_visual_contexts(viewport_paintable, forced_incompatible_rebuild);
    auto visual_context_tree = materialize_rust_main_visual_context_tree(viewport_paintable);
    if (is_compatible) {
        visual_context_tree.reuse_version_from(*m_visual_context_tree);
    } else {
        viewport_paintable.document().set_needs_to_record_display_list();
    }
    m_visual_context_tree = move(visual_context_tree);
    m_visual_context_tree_needs_compositor_update = true;
}

bool DocumentPaintState::update_accumulated_visual_context_values(ViewportPaintable& viewport_paintable, Paintable& paintable_box)
{
    if (!m_visual_context_tree.has_value())
        return false;
    if (m_force_incompatible_visual_context_tree_rebuild_for_testing)
        return false;
    if (!rust_update_accumulated_visual_context_values(viewport_paintable, paintable_box))
        return false;
    patch_rust_visual_context_nodes(viewport_paintable, *m_visual_context_tree, paintable_box.visual_context_nodes_begin(), paintable_box.visual_context_nodes_end());
    m_visual_context_tree_needs_compositor_update = true;
    return true;
}

void DocumentPaintState::update_visual_viewport_accumulated_visual_context(ViewportPaintable& viewport_paintable)
{
    if (!m_visual_context_tree.has_value()) {
        assign_accumulated_visual_contexts(viewport_paintable);
        return;
    }
    rust_update_visual_viewport_transform(viewport_paintable);
    patch_rust_visual_context_nodes(viewport_paintable, *m_visual_context_tree, VISUAL_VIEWPORT_NODE_INDEX.value(), VISUAL_VIEWPORT_NODE_INDEX.value() + 1);
    m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::append_paint_command_cache_source_resources(DisplayListResourceSet& retained_resources) const
{
    retained_resources.include(m_paint_command_cache_source_referenced_resources);
}

void DocumentPaintState::invalidate_all_cached_paint(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_invalidate_all_paint_caches(m_layout_node_arena->handle());
    Painting::set_needs_repaint(viewport_paintable.layout_node());
}

void DocumentPaintState::refresh_scroll_state(ViewportPaintable& viewport_paintable)
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;
    // https://drafts.csswg.org/css-position/#sticky-pos
    rust_refresh_scroll_state(viewport_paintable);
    m_scroll_state_snapshot = rust_scroll_state_snapshot(viewport_paintable);
}

void DocumentPaintState::reset_selection_states(ViewportPaintable& viewport_paintable)
{
    Layout::RustFFI::layout_arena_selection_clear(m_layout_node_arena->handle(), viewport_paintable.rust_slot());
}

void DocumentPaintState::recompute_selection_states(ViewportPaintable& viewport_paintable, DOM::Range& range)
{
    Vector<Layout::RustFFI::FfiSelectionEntry> entries;
    auto set_selection_state_on_all_slices = [&](DOM::Node& container, SelectionState state) {
        if (auto* text = as_if<DOM::Text>(container)) {
            Layout::TextOffsetMapping mapping { *text };
            mapping.for_each_fragment([&](Layout::TextNode const& slice) {
                entries.append({
                    .is_text_node_entry = true,
                    .layout_node = Layout::Node::slot_id(&slice),
                    .paintable = {},
                    .state = to_underlying(state),
                });
            });
            return;
        }
        if (auto* layout_node = container.unsafe_layout_node()) {
            if (auto paintable = layout_node->paintable()) {
                entries.append({
                    .is_text_node_entry = false,
                    .layout_node = {},
                    .paintable = paintable->rust_slot(),
                    .state = to_underlying(state),
                });
            }
        }
    };
    auto apply_entries = [&] {
        Layout::RustFFI::layout_arena_selection_apply(m_layout_node_arena->handle(), viewport_paintable.rust_slot(), entries.data(), entries.size(), range.start_offset(), range.end_offset());
    };

    // https://drafts.csswg.org/css-ui/#valdef-user-select-none
    // "The content of the element must be excluded from selection by [...] the selection methods of the Selection API
    // and the like." We honor this by leaving such nodes at SelectionState::None — even when they fall inside the
    // range. So, the selection highlight skips them.
    auto is_excluded_from_selection = [](DOM::Node const& node) {
        if (node.is_inert())
            return true;
        auto const* layout = node.unsafe_layout_node();
        return layout && layout->user_select_used_value() == CSS::UserSelect::None;
    };

    auto start_container = range.start_container();
    auto end_container = range.end_container();

    // 2. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range.start_offset() == range.end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            apply_entries();
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container) && !is_excluded_from_selection(*start_container)) {
            set_selection_state_on_all_slices(*start_container, SelectionState::StartAndEnd);
            apply_entries();
            return;
        }
    }

    // 3. Mark the selection start node as Start (if text) or Full (if anything else).
    if (!is_excluded_from_selection(*start_container) && start_container->unsafe_layout_node()) {
        if (is<DOM::Text>(*start_container))
            set_selection_state_on_all_slices(*start_container, SelectionState::Start);
        else
            set_selection_state_on_all_slices(*start_container, SelectionState::Full);
    }

    // 4. Mark the nodes between the start and end of the selection as Full.
    auto* start_at = start_container->child_at_index(range.start_offset());
    // If the start container has no child at that index, we need to start on the node right after the start container.
    if (!start_at) {
        if (auto* last_child = start_container->last_child()) {
            start_at = last_child->next_in_pre_order();
        } else {
            start_at = start_container->next_in_pre_order();
        }
    }

    DOM::Node* stop_at = end_container->child_at_index(range.end_offset());
    // Only stop at the end container if it has no children that may need to be included.
    for (auto* node = start_at; node && (node != stop_at && !(node == end_container.ptr() && !end_container->has_children())); node = node->next_in_pre_order(end_container.ptr())) {
        if (is_excluded_from_selection(*node))
            continue;
        set_selection_state_on_all_slices(*node, SelectionState::Full);
    }

    // 5. Mark the selection end node as End if it is a text node.
    if (!is_excluded_from_selection(*end_container) && is<DOM::Text>(*end_container) && end_container->unsafe_layout_node()) {
        set_selection_state_on_all_slices(*end_container, SelectionState::End);
    }

    apply_entries();
}

}
