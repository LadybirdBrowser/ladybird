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
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/SvgPaintResources.h>

namespace Web::Painting {

DocumentPaintState::DocumentPaintState(Layout::NodeArena& layout_node_arena)
    : m_layout_node_arena(layout_node_arena)
{
}

void DocumentPaintState::ensure_visual_context_tree(DOM::Document const& document) const
{
    const_cast<DOM::Document&>(document).update_paint_and_hit_testing_properties_if_needed();
}

bool DocumentPaintState::has_visual_context_tree(Layout::BegunRead const& read) const
{
    return Layout::RustFFI::render_state_has_visual_context_tree(m_layout_node_arena->host(), &read);
}

Compositing::AccumulatedVisualContextTree DocumentPaintState::visual_context_tree_without_update(Layout::BegunRead const& read, DOM::Document const& document) const
{
    return Compositing::AccumulatedVisualContextTree::adopt_rust_handle(retain_rust_main_visual_context_tree(read, document));
}

Compositing::AccumulatedVisualContextTree DocumentPaintState::visual_context_tree(DOM::Document const& document) const
{
    ensure_visual_context_tree(document);
    // The tree is the caller's own read of the paint state.
    Layout::ForcedReadScope read { document, false };
    return visual_context_tree_without_update(read, document);
}

u64 DocumentPaintState::visual_context_tree_structural_epoch(Layout::BegunRead const& read, DOM::Document const& document) const
{
    ensure_visual_context_tree(document);
    return visual_context_tree_structural_epoch_without_update(read);
}

u64 DocumentPaintState::visual_context_tree_structural_epoch_without_update(Layout::BegunRead const& read) const
{
    return Layout::RustFFI::render_state_visual_context_tree_structural_epoch(m_layout_node_arena->host(), &read);
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

void DocumentPaintState::viewport_row_was_reset()
{
    m_scroll_state_snapshot = {};
    m_boxes_with_auto_content_visibility.clear();
    m_visual_context_tree_needs_compositor_update = false;
}

void DocumentPaintState::update_accumulated_visual_contexts(Layout::BegunRead const& read, DOM::Document& document)
{
    bool svg_paint_resources_changed = sync_svg_paint_resources(read, document);
    auto result = rust_update_accumulated_visual_contexts(read, document);
    if (result.performed_full_build)
        ++m_accumulated_visual_context_tree_build_count;
    else
        ++m_accumulated_visual_context_tree_incremental_update_count;
    if (result.requires_display_list_recording || svg_paint_resources_changed)
        document.set_needs_to_record_display_list();
    m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::update_visual_viewport_accumulated_visual_context(Layout::BegunRead const& read, DOM::Document& document)
{
    if (!has_visual_context_tree(read)) {
        update_accumulated_visual_contexts(read, document);
        return;
    }
    rust_update_visual_viewport_transform(read, document);
    m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::begin_compositor_animation_update(DOM::Document& document)
{
    ensure_visual_context_tree(document);
    Layout::RustFFI::render_state_begin_compositor_animation_update(m_layout_node_arena->host());
}

void DocumentPaintState::publish_compositor_animations(Layout::BegunRead const& read, DOM::Document& document, PublishPendingCompositorAnimations publish_pending)
{
    ensure_visual_context_tree(document);
    auto outcome = Layout::RustFFI::render_state_publish_compositor_animations(m_layout_node_arena->host(), &read, publish_pending == PublishPendingCompositorAnimations::Yes);
    if (!outcome.published)
        return;
    m_visual_context_tree_needs_compositor_update = true;
    if (outcome.parameters_changed)
        ++document.style_invalidation_counters().compositor_visual_animation_updates;
    if (outcome.timing_anchors_changed)
        ++document.style_invalidation_counters().compositor_visual_animation_timing_anchor_updates;
}

void DocumentPaintState::republish_visual_animations(Layout::BegunRead const& read, DOM::Document& document)
{
    if (!Layout::RustFFI::render_state_visual_context_tree_has_visual_animations(m_layout_node_arena->host(), &read))
        return;
    m_visual_context_tree_needs_compositor_update = true;
    ++document.style_invalidation_counters().compositor_visual_animation_updates;
}

void DocumentPaintState::append_paint_command_cache_source_resources(Compositing::DisplayListResourceSet& retained_resources) const
{
    retained_resources.include(m_paint_command_cache_source_referenced_resources);
}

void DocumentPaintState::invalidate_all_cached_paint(DOM::Document& document)
{
    // The caller's own read of the render state.
    Layout::ForcedReadScope read { document, false };
    Layout::RustFFI::render_state_invalidate_all_paint_caches(m_layout_node_arena->host());
    Painting::set_needs_repaint(*document.unsafe_layout_node(read));
}

void DocumentPaintState::refresh_scroll_state(Layout::BegunRead const& read, DOM::Document& document)
{
    if (rust_refresh_scroll_state(read, document, m_scroll_state_snapshot))
        return;

    // LIBWEB_VERIFY_SCROLL_STATE: a skipped refresh must have been skippable. Every producer of a
    // scroll offset invalidates the state, so re-deriving the snapshot from scratch has to
    // reproduce the one kept.
    static bool const verify_scroll_state = getenv("LIBWEB_VERIFY_SCROLL_STATE") != nullptr;
    if (!verify_scroll_state)
        return;
    Compositing::ScrollStateSnapshot rederived_snapshot;
    rust_refresh_scroll_state(read, document, rederived_snapshot, ForceScrollStateRefresh::Yes);
    VERIFY(rederived_snapshot.device_offsets() == m_scroll_state_snapshot.device_offsets());
}

void DocumentPaintState::reset_selection_states(Layout::BegunRead const& read, DOM::Document& document)
{
    Layout::RustFFI::render_state_clear_selection(m_layout_node_arena->host(), viewport_row_slot(read, document));
}

static void append_highlight_entry(Layout::BegunRead const& read, Vector<Layout::RustFFI::FfiSelectionEntry>& entries, DOM::Node& container, SelectionState state)
{
    if (is<DOM::Text>(container)) {
        if (auto* layout_node = container.unsafe_layout_node(read)) {
            entries.append({
                .is_text_node_entry = true,
                .layout_node = Layout::Node::slot_id(layout_node),
                .state = to_underlying(state),
            });
        }
        return;
    }
    if (auto* layout_node = container.unsafe_layout_node(read)) {
        if (has_committed_box(*layout_node)) {
            entries.append({
                .is_text_node_entry = false,
                .layout_node = Layout::Node::slot_id(layout_node),
                .state = to_underlying(state),
            });
        }
    }
}

template<typename IsExcluded, typename Callback>
static void for_each_node_in_highlight_range(Layout::BegunRead const& read, DOM::Range& range, IsExcluded is_excluded, Callback callback)
{
    auto start_container = range.start_container();
    auto end_container = range.end_container();

    // 2. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range.start_offset() == range.end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container) && !is_excluded(*start_container)) {
            callback(*start_container, SelectionState::StartAndEnd);
            return;
        }
    }

    // 3. Mark the selection start node as Start (if text) or Full (if anything else).
    if (!is_excluded(*start_container) && start_container->unsafe_layout_node(read)) {
        if (is<DOM::Text>(*start_container))
            callback(*start_container, SelectionState::Start);
        else
            callback(*start_container, SelectionState::Full);
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
        if (is_excluded(*node))
            continue;
        callback(*node, SelectionState::Full);
    }

    // 5. Mark the selection end node as End if it is a text node.
    if (!is_excluded(*end_container) && is<DOM::Text>(*end_container) && end_container->unsafe_layout_node(read)) {
        callback(*end_container, SelectionState::End);
    }
}

void DocumentPaintState::recompute_selection_states(Layout::BegunRead const& read, DOM::Document& document, DOM::Range& range)
{
    // https://drafts.csswg.org/css-ui/#valdef-user-select-none
    // "The content of the element must be excluded from selection by [...] the selection methods of the Selection API
    // and the like." We honor this by leaving such nodes at SelectionState::None — even when they fall inside the
    // range. So, the selection highlight skips them.
    auto is_excluded_from_selection = [&read](DOM::Node const& node) {
        if (node.is_inert())
            return true;
        auto const* layout = node.unsafe_layout_node(read);
        return layout && layout->user_select_used_value() == CSS::UserSelect::None;
    };

    Vector<Layout::RustFFI::FfiSelectionEntry> entries;
    for_each_node_in_highlight_range(read, range, is_excluded_from_selection, [&](DOM::Node& node, SelectionState state) {
        append_highlight_entry(read, entries, node, state);
    });
    Layout::RustFFI::render_state_apply_selection(m_layout_node_arena->host(), viewport_row_slot(read, document), entries.data(), entries.size(), range.start_offset(), range.end_offset());
}

void DocumentPaintState::reset_search_text_states()
{
    Layout::RustFFI::render_state_clear_search_text(m_layout_node_arena->host());
}

void DocumentPaintState::recompute_search_text_states(Layout::BegunRead const& read, DOM::Document& document, DOM::Range& range)
{
    auto is_excluded_from_search_text = [](DOM::Node const&) { return false; };

    Vector<Layout::RustFFI::FfiSelectionEntry> entries;
    for_each_node_in_highlight_range(read, range, is_excluded_from_search_text, [&](DOM::Node& node, SelectionState state) {
        if (is<DOM::Text>(node))
            append_highlight_entry(read, entries, node, state);
    });
    Layout::RustFFI::render_state_apply_search_text(m_layout_node_arena->host(), viewport_row_slot(read, document), entries.data(), entries.size(), range.start_offset(), range.end_offset());
}

}
