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
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Painting {

DocumentPaintState::DocumentPaintState(Layout::NodeArena& layout_node_arena)
    : m_layout_node_arena(layout_node_arena)
{
}

void DocumentPaintState::ensure_visual_context_tree(DOM::Document const& document) const
{
    const_cast<DOM::Document&>(document).update_paint_and_hit_testing_properties_if_needed();
}

bool DocumentPaintState::has_visual_context_tree() const
{
    return Layout::RustFFI::layout_arena_has_visual_context_tree(m_layout_node_arena->handle());
}

AccumulatedVisualContextTree DocumentPaintState::visual_context_tree_without_update(DOM::Document const& document) const
{
    auto tree = AccumulatedVisualContextTree::adopt_rust_handle(retain_rust_main_visual_context_tree(document));
    tree.set_visual_animations(m_visual_context_tree_visual_animations);
    return tree;
}

AccumulatedVisualContextTree DocumentPaintState::visual_context_tree(DOM::Document const& document) const
{
    ensure_visual_context_tree(document);
    return visual_context_tree_without_update(document);
}

u64 DocumentPaintState::visual_context_tree_structural_epoch(DOM::Document const& document) const
{
    ensure_visual_context_tree(document);
    return Layout::RustFFI::layout_arena_visual_context_tree_structural_epoch(m_layout_node_arena->handle());
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

void DocumentPaintState::viewport_row_was_reset(DOM::Document& document)
{
    clear_scroll_state(document);
    m_boxes_with_auto_content_visibility.clear();
    m_visual_context_tree_needs_compositor_update = false;
}

void DocumentPaintState::build_stacking_context_tree_if_needed(DOM::Document& document)
{
    if (m_stacking_context_tree_is_valid)
        return;
    rust_build_stacking_context_tree(document);
    m_stacking_context_tree_is_valid = true;
}

void DocumentPaintState::invalidate_stacking_context_tree()
{
    m_stacking_context_tree_is_valid = false;
}

void DocumentPaintState::refresh_sticky_constraints(DOM::Document& document)
{
    m_needs_to_refresh_scroll_state = true;
    if (mirror_rust_refresh_sticky_constraints(document))
        m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::set_needs_to_refresh_scroll_state(DOM::Document& document, bool value)
{
    m_needs_to_refresh_scroll_state = value;
    mirror_rust_set_needs_to_refresh_scroll_state(document, value);
}

void DocumentPaintState::clear_scroll_state(DOM::Document& document)
{
    m_scroll_state_snapshot = {};
    m_needs_to_refresh_scroll_state = true;
    mirror_rust_clear_scroll_state(document);
}

void DocumentPaintState::update_accumulated_visual_contexts(DOM::Document& document)
{
    auto result = rust_update_accumulated_visual_contexts(document);
    if (result.performed_full_build) {
        m_scroll_state_snapshot = {};
        ++m_accumulated_visual_context_tree_build_count;
    } else {
        ++m_accumulated_visual_context_tree_incremental_update_count;
    }
    set_needs_to_refresh_scroll_state(document, true);
    if (result.requires_display_list_recording)
        document.set_needs_to_record_display_list();
    if (result.structural_epoch_changed)
        m_visual_context_tree_visual_animations = nullptr;
    m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::update_visual_viewport_accumulated_visual_context(DOM::Document& document)
{
    if (!has_visual_context_tree()) {
        update_accumulated_visual_contexts(document);
        return;
    }
    rust_update_visual_viewport_transform(document);
    m_visual_context_tree_needs_compositor_update = true;
}

void DocumentPaintState::set_visual_animations(DOM::Document& document, Vector<Compositor::VisualAnimation> animations)
{
    ensure_visual_context_tree(document);
    auto published_animations = m_visual_context_tree_visual_animations ? m_visual_context_tree_visual_animations->animations.span() : ReadonlySpan<Compositor::VisualAnimation> {};
    if (published_animations == animations.span())
        return;
    bool animation_parameters_changed = m_visual_animations.size() != animations.size();
    if (!animation_parameters_changed) {
        for (size_t index = 0; index < animations.size(); ++index) {
            if (!m_visual_animations[index].has_same_animation_parameters(animations[index])) {
                animation_parameters_changed = true;
                break;
            }
        }
    }
    m_visual_animations = animations;
    if (animations.is_empty()) {
        m_visual_context_tree_visual_animations = nullptr;
    } else {
        m_visual_context_tree_visual_animations = adopt_ref(*new VisualAnimationList(move(animations)));
    }
    m_visual_context_tree_needs_compositor_update = true;
    if (animation_parameters_changed)
        ++document.style_invalidation_counters().compositor_visual_animation_updates;
}

void DocumentPaintState::append_paint_command_cache_source_resources(DisplayListResourceSet& retained_resources) const
{
    retained_resources.include(m_paint_command_cache_source_referenced_resources);
}

void DocumentPaintState::invalidate_all_cached_paint(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_invalidate_all_paint_caches(m_layout_node_arena->handle());
    Painting::set_needs_repaint(*document.unsafe_layout_node());
}

void DocumentPaintState::refresh_scroll_state(DOM::Document& document)
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;
    // https://drafts.csswg.org/css-position/#sticky-pos
    rust_refresh_scroll_state(document);
    m_scroll_state_snapshot = rust_scroll_state_snapshot(document);
    if (has_visual_context_tree())
        resolve_sticky_offsets(visual_context_tree_without_update(document), m_scroll_state_snapshot);
}

void DocumentPaintState::reset_selection_states(DOM::Document& document)
{
    Layout::RustFFI::layout_arena_selection_clear(m_layout_node_arena->handle(), viewport_row_slot(document));
}

void DocumentPaintState::recompute_selection_states(DOM::Document& document, DOM::Range& range)
{
    Vector<Layout::RustFFI::FfiSelectionEntry> entries;
    auto set_selection_state_on_all_slices = [&](DOM::Node& container, SelectionState state) {
        if (auto* text = as_if<DOM::Text>(container)) {
            Layout::TextOffsetMapping mapping { *text };
            mapping.for_each_fragment([&](Layout::TextNode const& slice) {
                entries.append({
                    .is_text_node_entry = true,
                    .layout_node = Layout::Node::slot_id(&slice),
                    .state = to_underlying(state),
                });
            });
            return;
        }
        if (auto* layout_node = container.unsafe_layout_node()) {
            if (has_committed_box(*layout_node)) {
                entries.append({
                    .is_text_node_entry = false,
                    .layout_node = Layout::Node::slot_id(layout_node),
                    .state = to_underlying(state),
                });
            }
        }
    };
    auto apply_entries = [&] {
        Layout::RustFFI::layout_arena_selection_apply(m_layout_node_arena->handle(), viewport_row_slot(document), entries.data(), entries.size(), range.start_offset(), range.end_offset());
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
