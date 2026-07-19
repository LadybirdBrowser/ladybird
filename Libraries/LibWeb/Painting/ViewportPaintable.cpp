/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

AccumulatedVisualContextTree build_accumulated_visual_context_tree(ViewportPaintable&);
bool update_accumulated_visual_context_values(ViewportPaintable&, Paintable&);
void update_visual_viewport_accumulated_visual_context(ViewportPaintable&);

NonnullRefPtr<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return adopt_ref(*new ViewportPaintable(layout_viewport));
}

ViewportPaintable::ViewportPaintable(Layout::Viewport const& layout_viewport)
    : PaintableWithLines(layout_viewport)
{
}

ViewportPaintable::~ViewportPaintable() = default;

void ViewportPaintable::ensure_visual_context_tree() const
{
    const_cast<DOM::Document&>(document()).update_paint_and_hit_testing_properties_if_needed();
}

AccumulatedVisualContextTree const& ViewportPaintable::visual_context_tree() const
{
    ensure_visual_context_tree();
    VERIFY(m_visual_context_tree.has_value());
    return *m_visual_context_tree;
}

AccumulatedVisualContextTree& ViewportPaintable::visual_context_tree()
{
    ensure_visual_context_tree();
    VERIFY(m_visual_context_tree.has_value());
    return *m_visual_context_tree;
}

struct BlockingWheelEventRegionState {
    bool has_blocking_wheel_event_listeners { false };
    bool has_blocking_wheel_event_region_covering_viewport { false };
};

static BlockingWheelEventRegionState collect_root_blocking_wheel_event_regions(DOM::Document& document)
{
    DOM::EventTarget* roots[] = {
        document.navigable() ? document.navigable()->active_window() : nullptr,
        &document,
        document.document_element(),
        document.body(),
    };
    for (auto* target : roots) {
        if (target && target->has_blocking_wheel_event_listener()) {
            return {
                .has_blocking_wheel_event_listeners = true,
                .has_blocking_wheel_event_region_covering_viewport = true,
            };
        }
    }
    return {};
}

void ViewportPaintable::initialize_async_scrolling_metadata_recording(DisplayListRecordingContext& context)
{
    auto blocking_wheel_event_region_state = collect_root_blocking_wheel_event_regions(document());
    context.set_async_scrolling_metadata_context(
        document().unique_id(),
        visual_context_tree(),
        scroll_state(),
        blocking_wheel_event_region_state.has_blocking_wheel_event_listeners,
        blocking_wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport);
}

void ViewportPaintable::finalize_async_scrolling_metadata_recording(DisplayListRecordingContext& context, HTML::LocalNavigable& navigable, Gfx::IntRect viewport_rect)
{
    if (!context.is_recording_async_scrolling_metadata())
        return;

    context.display_list_recorder().set_async_scrolling_metadata({
        .viewport_rect = viewport_rect,
        .wheel_event_listener_state_generation = navigable.page().wheel_event_listener_state_generation(),
        .has_blocking_wheel_event_listeners = context.has_blocking_wheel_event_listeners(),
        .has_blocking_wheel_event_region_covering_viewport = context.has_blocking_wheel_event_region_covering_viewport(),
    });
}

void ViewportPaintable::reset_for_relayout()
{
    PaintableWithLines::reset_for_relayout();
    clear_scroll_state();
    m_paintable_boxes_with_auto_content_visibility.clear();
    m_visual_context_tree.clear();
    m_visual_context_tree_node_count_without_inspector_overlays = 0;
    m_visual_context_tree_needs_compositor_update = false;
}

void ViewportPaintable::build_stacking_context_tree_if_needed()
{
    if (stacking_context())
        return;
    build_stacking_context_tree();
}

void ViewportPaintable::build_stacking_context_tree()
{
    set_stacking_context(StackingContext::create(*this, nullptr, 0));

    Vector<PaintableWithLines*> blocks_with_inline_box_pieces;
    size_t index_in_tree_order = 1;
    for_each_in_subtree_of_type<Paintable>([&](auto& paintable_box) {
        paintable_box.invalidate_stacking_context();
        if (auto* paintable_with_lines = as_if<PaintableWithLines>(paintable_box); paintable_with_lines && !paintable_with_lines->inline_box_pieces().is_empty())
            blocks_with_inline_box_pieces.append(paintable_with_lines);
        auto parent_context = paintable_box.enclosing_stacking_context();
        auto establishes_stacking_context = paintable_box.layout_node().establishes_stacking_context();
        if ((paintable_box.is_positioned() || establishes_stacking_context) && paintable_box.effective_z_index().value_or(0) == 0)
            parent_context->m_positioned_descendants_and_stacking_contexts_with_stack_level_0.append(paintable_box);
        if (!paintable_box.is_positioned() && paintable_box.is_floating())
            parent_context->m_non_positioned_floating_descendants.append(paintable_box);
        if (!establishes_stacking_context && (paintable_box.is_inline() || is<Layout::ReplacedBox>(paintable_box.layout_node())))
            parent_context->m_contains_inline_or_replaced_descendants = true;
        if (!establishes_stacking_context) {
            VERIFY(!paintable_box.stacking_context());
            return TraversalDecision::Continue;
        }
        VERIFY(parent_context);
        paintable_box.set_stacking_context(StackingContext::create(paintable_box, parent_context, index_in_tree_order++));
        return TraversalDecision::Continue;
    });

    stacking_context()->sort();

    for (auto* block : blocks_with_inline_box_pieces)
        block->assign_fragment_ownership();
}

void ViewportPaintable::paint_all_phases(DisplayListRecordingContext& context)
{
    build_stacking_context_tree_if_needed();
    context.display_list_recorder().save_layer();
    stacking_context()->paint(context);
    context.display_list_recorder().restore();
}

void ViewportPaintable::precompute_sticky_constraints(ScrollStateSlot sticky_slot, Paintable const& paintable_box)
{
    auto nearest_scrolling_ancestor_slot = m_scroll_state.nearest_scrolling_ancestor_slot(sticky_slot);
    if (nearest_scrolling_ancestor_slot == NO_SCROLL_STATE_SLOT)
        return;

    auto scroll_ancestor_paintable_ref = m_scroll_state.frame_at_slot(nearest_scrolling_ancestor_slot).paintable_box_if_alive();
    if (!scroll_ancestor_paintable_ref)
        return;
    auto const& scroll_ancestor_paintable = *scroll_ancestor_paintable_ref;
    auto sticky_border_box_rect = paintable_box.absolute_border_box_rect();
    RefPtr<Paintable const> containing_block_of_sticky = paintable_box.containing_block();

    CSSPixelRect containing_block_region;
    bool needs_parent_offset_adjustment = false;
    if (containing_block_of_sticky == scroll_ancestor_paintable_ref) {
        containing_block_region = { {}, containing_block_of_sticky->scrollable_overflow_rect()->size() };
    } else {
        containing_block_region = containing_block_of_sticky->absolute_border_box_rect()
                                      .translated(-scroll_ancestor_paintable.absolute_rect().top_left());
        needs_parent_offset_adjustment = true;
    }

    m_scroll_state.frame_at_slot(sticky_slot).set_sticky_constraints({
        .position_relative_to_scroll_ancestor = sticky_border_box_rect.top_left() - scroll_ancestor_paintable.absolute_rect().top_left(),
        .border_box_size = sticky_border_box_rect.size(),
        .scrollport_size = scroll_ancestor_paintable.absolute_rect().size(),
        .containing_block_region = containing_block_region,
        .needs_parent_offset_adjustment = needs_parent_offset_adjustment,
        .insets = paintable_box.sticky_insets(),
    });
}

void ViewportPaintable::refresh_sticky_constraints()
{
    m_scroll_state.for_each_sticky_frame([&](ScrollStateSlot slot, ScrollFrame& frame) {
        // Skip frames whose paintables a subtree relayout has replaced; the pending visual context
        // tree rebuild recreates those frames before anything reads them.
        if (auto paintable_box = frame.paintable_box_if_alive())
            precompute_sticky_constraints(slot, *paintable_box);
    });
    m_needs_to_refresh_scroll_state = true;
}

void ViewportPaintable::clear_scroll_state()
{
    m_scroll_state.clear();
    m_scroll_state_snapshot = {};
    m_needs_to_refresh_scroll_state = true;
}

void ViewportPaintable::register_scroll_frame(AccumulatedVisualContextTree& visual_context_tree_being_built, VisualContextIndex node_index, Paintable const& paintable_box, VisualContextIndex parent_index)
{
    auto slot = m_scroll_state.register_scroll_frame(node_index, paintable_box, visual_context_tree_being_built.scroll_state_slot_for_node(parent_index));
    visual_context_tree_being_built.node_at(node_index).data.get<ScrollData>().state_slot = slot;
}

void ViewportPaintable::register_sticky_frame(AccumulatedVisualContextTree& visual_context_tree_being_built, VisualContextIndex node_index, Paintable const& paintable_box, VisualContextIndex parent_index)
{
    auto slot = m_scroll_state.register_sticky_frame(node_index, paintable_box, visual_context_tree_being_built.scroll_state_slot_for_node(parent_index));
    visual_context_tree_being_built.node_at(node_index).data.get<ScrollData>().state_slot = slot;
    precompute_sticky_constraints(slot, paintable_box);
}

CSSPixelPoint ViewportPaintable::cumulative_scroll_offset_for_node(VisualContextIndex scroll_node_index) const
{
    return m_scroll_state.cumulative_offset(visual_context_tree().scroll_state_slot_for_node(scroll_node_index));
}

void ViewportPaintable::assign_accumulated_visual_contexts()
{
    clear_scroll_state();
    auto visual_context_tree = build_accumulated_visual_context_tree(*this);
    ++m_accumulated_visual_context_tree_build_count;
    auto is_compatible = m_visual_context_tree.has_value() && visual_context_tree.is_compatible_with(*m_visual_context_tree);
    if (m_force_incompatible_visual_context_tree_rebuild_for_testing) {
        m_force_incompatible_visual_context_tree_rebuild_for_testing = false;
        is_compatible = false;
    }
    if (is_compatible) {
        visual_context_tree.reuse_version_from(*m_visual_context_tree);
    } else {
        document().set_needs_to_record_display_list();
    }
    m_visual_context_tree = move(visual_context_tree);
    m_visual_context_tree_node_count_without_inspector_overlays = m_visual_context_tree->nodes().size();
    m_visual_context_tree_needs_compositor_update = true;
}

bool ViewportPaintable::update_accumulated_visual_context_values(Paintable& paintable_box)
{
    if (!m_visual_context_tree.has_value())
        return false;
    if (m_force_incompatible_visual_context_tree_rebuild_for_testing)
        return false;
    if (!Painting::update_accumulated_visual_context_values(*this, paintable_box))
        return false;
    m_visual_context_tree_needs_compositor_update = true;
    return true;
}

void ViewportPaintable::update_visual_viewport_accumulated_visual_context()
{
    if (!m_visual_context_tree.has_value()) {
        assign_accumulated_visual_contexts();
        return;
    }
    Painting::update_visual_viewport_accumulated_visual_context(*this);
    m_visual_context_tree_needs_compositor_update = true;
}

// Painting inspector overlays appends the highlighted elements' transform/scroll ancestor contexts onto the visual
// context tree (see Paintable::paint_with_inspector_overlay_context). Those nodes are only referenced by the display
// list recorded alongside them — so each new recording prunes the ones appended by the previous recording before
// appending its own; otherwise they would accumulate without bound.
void ViewportPaintable::prune_inspector_overlay_visual_contexts()
{
    if (!m_visual_context_tree.has_value())
        return;
    m_visual_context_tree->shrink(m_visual_context_tree_node_count_without_inspector_overlays);
}

void ViewportPaintable::invalidate_all_cached_paint()
{
    for_each_in_inclusive_subtree([](auto& paintable) {
        paintable.invalidate_paint_cache();
        return TraversalDecision::Continue;
    });
    set_needs_repaint();
}

void ViewportPaintable::refresh_scroll_state()
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;

    // https://drafts.csswg.org/css-position/#sticky-pos
    // Sticky positioning is similar to relative positioning except the offsets are automatically calculated
    // in reference to the nearest scrollport.
    m_scroll_state.for_each_sticky_frame([&](auto slot, auto& frame) {
        auto nearest_scrolling_ancestor_slot = m_scroll_state.nearest_scrolling_ancestor_slot(slot);
        if (nearest_scrolling_ancestor_slot == NO_SCROLL_STATE_SLOT || !frame.has_sticky_constraints())
            return;

        auto const& sticky_data = frame.sticky_constraints();
        auto const& sticky_insets = sticky_data.insets;
        auto scroll_ancestor_paintable_ref = m_scroll_state.frame_at_slot(nearest_scrolling_ancestor_slot).paintable_box_if_alive();
        if (!scroll_ancestor_paintable_ref)
            return;
        auto const& scroll_ancestor_paintable = *scroll_ancestor_paintable_ref;

        // NB: For nested sticky elements, work in the coordinate space where the sticky ancestors are
        //     at their current positions. The scrolling ancestor's offset is already represented by
        //     scrollport_rect and must not be applied to the sticky position a second time.
        auto parent_sticky_offset = m_scroll_state.cumulative_sticky_offset(frame.parent_slot());

        auto sticky_position_in_ancestor = sticky_data.position_relative_to_scroll_ancestor + parent_sticky_offset;

        auto containing_block_region = sticky_data.containing_block_region;
        if (sticky_data.needs_parent_offset_adjustment)
            containing_block_region.translate_by(parent_sticky_offset);
        CSSPixelPoint min_offset_within_containing_block = containing_block_region.top_left();
        CSSPixelPoint max_offset_within_containing_block = {
            containing_block_region.right() - sticky_data.border_box_size.width(),
            containing_block_region.bottom() - sticky_data.border_box_size.height()
        };

        CSSPixelRect scrollport_rect { scroll_ancestor_paintable.scroll_offset(), sticky_data.scrollport_size };
        CSSPixelPoint sticky_offset;

        if (sticky_insets.top.has_value()) {
            if (scrollport_rect.top() > sticky_position_in_ancestor.y() - *sticky_insets.top)
                sticky_offset.set_y(min(scrollport_rect.top() + *sticky_insets.top, max_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_insets.left.has_value()) {
            if (scrollport_rect.left() > sticky_position_in_ancestor.x() - *sticky_insets.left)
                sticky_offset.set_x(min(scrollport_rect.left() + *sticky_insets.left, max_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }
        if (sticky_insets.bottom.has_value()) {
            if (scrollport_rect.bottom() < sticky_position_in_ancestor.y() + sticky_data.border_box_size.height() + *sticky_insets.bottom)
                sticky_offset.set_y(max(scrollport_rect.bottom() - sticky_data.border_box_size.height() - *sticky_insets.bottom, min_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_insets.right.has_value()) {
            if (scrollport_rect.right() < sticky_position_in_ancestor.x() + sticky_data.border_box_size.width() + *sticky_insets.right)
                sticky_offset.set_x(max(scrollport_rect.right() - sticky_data.border_box_size.width() - *sticky_insets.right, min_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }

        frame.set_own_offset(sticky_offset);
    });

    m_scroll_state.for_each_scroll_frame([&](auto, auto& frame) {
        if (auto paintable_box = frame.paintable_box_if_alive())
            frame.set_own_offset(-paintable_box->scroll_offset());
    });

    m_scroll_state_snapshot = m_scroll_state.snapshot(document().page().client().device_pixels_per_css_pixel());
}

GC::Ptr<Selection::Selection> ViewportPaintable::selection() const
{
    return document().get_selection();
}

void ViewportPaintable::reset_selection_states()
{
    for_each_in_inclusive_subtree([](auto& paintable) {
        paintable.set_selection_state(SelectionState::None);
        if (auto* paintable_with_lines = as_if<PaintableWithLines>(paintable))
            paintable_with_lines->reset_fragment_selection_states();
        return TraversalDecision::Continue;
    });
}

void ViewportPaintable::recompute_selection_states(DOM::Range& range)
{
    // 1. Start by resetting the selection state of all layout nodes to None.
    reset_selection_states();

    auto set_selection_state_on_all_slices = [](DOM::Node& container, SelectionState state) {
        if (auto* text = as_if<DOM::Text>(container)) {
            Layout::TextOffsetMapping mapping { *text };
            mapping.for_each_paintable_fragment([&](PaintableFragment& fragment) {
                fragment.set_selection_state(state);
                return TraversalDecision::Continue;
            });
            return;
        }
        if (auto* layout_node = container.unsafe_layout_node()) {
            if (auto paintable = layout_node->paintable())
                paintable->set_selection_state(state);
        }
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
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container) && !is_excluded_from_selection(*start_container)) {
            set_selection_state_on_all_slices(*start_container, SelectionState::StartAndEnd);
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
    for (auto* node = start_at; node && (node != stop_at && !(node == end_container && !end_container->has_children())); node = node->next_in_pre_order(end_container)) {
        if (is_excluded_from_selection(*node))
            continue;
        set_selection_state_on_all_slices(*node, SelectionState::Full);
    }

    // 5. Mark the selection end node as End if it is a text node.
    if (!is_excluded_from_selection(*end_container) && is<DOM::Text>(*end_container) && end_container->unsafe_layout_node()) {
        set_selection_state_on_all_slices(*end_container, SelectionState::End);
    }
}

bool ViewportPaintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double, double)
{
    return false;
}

}
