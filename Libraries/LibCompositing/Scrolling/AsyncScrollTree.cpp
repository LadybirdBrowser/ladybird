/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>

#include <AK/Debug.h>

namespace Compositing {

void AsyncScrollTree::set_state(AsyncScrollingState&& state)
{
    m_document_id = state.document_id;
    m_scroll_nodes = move(state.scroll_nodes);
    m_snap_containers = move(state.snap_containers);
    m_device_pixels_per_css_pixel = state.device_pixels_per_css_pixel;
    m_wheel_hit_test_regions = move(state.wheel_hit_test_targets);
    m_main_thread_wheel_event_regions = move(state.main_thread_wheel_event_regions);
    m_nested_context_wheel_regions = move(state.nested_context_wheel_regions);
    m_blocking_wheel_event_regions = move(state.blocking_wheel_event_regions);
    m_scroll_chaining_barriers = move(state.scroll_chaining_barriers);
    m_has_blocking_wheel_event_region_covering_viewport = state.has_blocking_wheel_event_region_covering_viewport;
    m_viewport_keeps_horizontal_scrolls = state.viewport_keeps_horizontal_scrolls;
    m_viewport_keeps_vertical_scrolls = state.viewport_keeps_vertical_scrolls;
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_nested_context_wheel_regions.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_cached_scroll_chaining_barriers.clear();
    m_visual_context_tree_structural_epoch.clear();
}

Web::InputRoutingDecision input_routing_decision_for_first_wheel_step(WheelHitTestOutcome outcome)
{
    switch (outcome) {
    case WheelHitTestOutcome::FoundScrollTarget:
        return Web::InputRoutingDecision::Latched;
    case WheelHitTestOutcome::NoScrollTarget:
        return Web::InputRoutingDecision::NoScrollTarget;
    case WheelHitTestOutcome::BlockedByMainThreadRegion:
        return Web::InputRoutingDecision::BlockedByMainThreadRegion;
    case WheelHitTestOutcome::BlockedByWheelEventRegion:
        return Web::InputRoutingDecision::BlockedByWheelEventRegion;
    case WheelHitTestOutcome::HitTestTargetsOfAnotherVisualContextTree:
        return Web::InputRoutingDecision::HitTestTargetsOfAnotherVisualContextTree;
    }
    VERIFY_NOT_REACHED();
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_id(AsyncScrollNodeID node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.node_id == node_id)
            return &node;
    }
    return nullptr;
}

static bool delta_is_mainly_vertical(Gfx::FloatPoint delta)
{
    return AK::abs(delta.y()) >= AK::abs(delta.x());
}

// https://drafts.csswg.org/css-overscroll-1/#overscroll-behavior-properties
// A step is taken by one scroller, and a touchpad rarely moves along one axis exactly, so the main axis of the step
// decides whether a scroller that cannot take it passes it on.
static bool node_chains_scroll_to_ancestors(AsyncScrollNode const& node, Gfx::FloatPoint delta)
{
    return delta_is_mainly_vertical(delta) ? node.chains_scroll_to_ancestors_vertically : node.chains_scroll_to_ancestors_horizontally;
}

WheelHitTestResult AsyncScrollTree::hit_test_result_for_scroll_node(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for, ReadonlySpan<AsyncScrollNodeID> scroll_nodes_behind_scroll_chaining_barriers) const
{
    auto can_take_step = [&](AsyncScrollNode const& node) {
        Optional<Gfx::FloatPoint> in_flight_destination;
        if (in_flight_destination_for)
            in_flight_destination = in_flight_destination_for(node.stable_node_id);
        return can_scroll_node_from_offset_by_delta(node, in_flight_destination.value_or(scroll_offset_for_node(node, m_scroll_state_snapshot)), delta);
    };

    for (auto const* node = scroll_node_for_id(node_id); node;) {
        // Only the main thread sees the box that keeps the scroll from this scroller, so it finds out whether the wheel
        // is under that box.
        if (scroll_nodes_behind_scroll_chaining_barriers.contains_slow(node->node_id))
            return { .outcome = WheelHitTestOutcome::BlockedByMainThreadRegion };
        if (can_take_step(*node))
            return { .outcome = WheelHitTestOutcome::FoundScrollTarget, .node_id = node->node_id, .innermost_scroll_node_id = node_id };
        if (!node_chains_scroll_to_ancestors(*node, delta))
            return { .outcome = WheelHitTestOutcome::NoScrollTarget, .innermost_scroll_node_id = node_id, .node_keeping_the_scroll = node->node_id };
        if (!node->parent_node_id.has_value())
            break;
        node = scroll_node_for_id(*node->parent_node_id);
    }
    return { .outcome = WheelHitTestOutcome::NoScrollTarget, .innermost_scroll_node_id = node_id };
}

Optional<AsyncScrollNodeID> AsyncScrollTree::outermost_scroll_node_wheel_scrollable_along_main_axis_of(AsyncScrollNodeID innermost_node_id, Gfx::FloatPoint delta) const
{
    Optional<AsyncScrollNodeID> outermost_node_id;
    auto node_id = innermost_node_id;
    for (size_t remaining_ancestors = m_scroll_nodes.size(); remaining_ancestors > 0; --remaining_ancestors) {
        auto const* node = scroll_node_for_id(node_id);
        if (!node)
            break;
        if (delta_is_mainly_vertical(delta) ? node->can_be_wheel_scrolled_vertically : node->can_be_wheel_scrolled_horizontally)
            outermost_node_id = node->node_id;
        if (!node_chains_scroll_to_ancestors(*node, delta) || !node->parent_node_id.has_value())
            break;
        node_id = *node->parent_node_id;
    }
    return outermost_node_id;
}

AsyncSnapContainer const* AsyncScrollTree::snap_container_for_node(AsyncScrollNodeID node_id) const
{
    for (auto const& snap_container : m_snap_containers) {
        if (snap_container.node_id == node_id)
            return &snap_container;
    }
    return nullptr;
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_stable_id(Web::AsyncScrollNodeStableID stable_node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.stable_node_id == stable_node_id)
            return &node;
    }
    return nullptr;
}

Gfx::FloatPoint AsyncScrollTree::clamp_scroll_offset_to_node(AsyncScrollNode const& node, Gfx::FloatPoint scroll_offset)
{
    scroll_offset.set_x(max(node.min_scroll_offset.x(), min(scroll_offset.x(), node.max_scroll_offset.x())));
    scroll_offset.set_y(max(node.min_scroll_offset.y(), min(scroll_offset.y(), node.max_scroll_offset.y())));
    return scroll_offset;
}

Gfx::FloatPoint AsyncScrollTree::scroll_offset_for_node(AsyncScrollNode const& node, Compositing::ScrollStateSnapshot const& scroll_state_snapshot)
{
    auto device_offset = scroll_state_snapshot.device_offset_for_index(node.node_id.scroll_node_index);
    return { -device_offset.x(), -device_offset.y() };
}

bool AsyncScrollTree::can_scroll_node_by_delta(AsyncScrollNode const& node, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta)
{
    return can_scroll_node_from_offset_by_delta(node, scroll_offset_for_node(node, scroll_state_snapshot), delta);
}

bool AsyncScrollTree::can_scroll_node_from_offset_by_delta(AsyncScrollNode const& node, Gfx::FloatPoint scroll_offset, Gfx::FloatPoint delta)
{
    if (node.can_be_wheel_scrolled_horizontally && delta.x() < 0 && scroll_offset.x() > node.min_scroll_offset.x())
        return true;
    if (node.can_be_wheel_scrolled_horizontally && delta.x() > 0 && scroll_offset.x() < node.max_scroll_offset.x())
        return true;
    if (node.can_be_wheel_scrolled_vertically && delta.y() < 0 && scroll_offset.y() > node.min_scroll_offset.y())
        return true;
    if (node.can_be_wheel_scrolled_vertically && delta.y() > 0 && scroll_offset.y() < node.max_scroll_offset.y())
        return true;
    return false;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scrollable_ancestor_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    while (node_chains_scroll_to_ancestors(*node, delta) && node->parent_node_id.has_value()) {
        node = scroll_node_for_id(*node->parent_node_id);
        if (!node)
            return {};
        if (can_scroll_node_by_delta(*node, scroll_state_snapshot, delta))
            return node->node_id;
    }
    return {};
}

Gfx::FloatPoint AsyncScrollTree::apply_scroll_delta_to_node(AsyncScrollNode const& node, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto old_scroll_offset = scroll_offset_for_node(node, scroll_state_snapshot);
    Gfx::FloatPoint wheel_scrollable_delta {
        node.can_be_wheel_scrolled_horizontally ? delta.x() : 0,
        node.can_be_wheel_scrolled_vertically ? delta.y() : 0,
    };
    auto new_scroll_offset = clamp_scroll_offset_to_node(node, old_scroll_offset.translated(wheel_scrollable_delta));
    if (new_scroll_offset == old_scroll_offset) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} did not move for delta {},{} (offset={},{} max={},{})",
            node.node_id.scroll_node_index.value(), delta.x(), delta.y(), old_scroll_offset.x(), old_scroll_offset.y(), node.max_scroll_offset.x(), node.max_scroll_offset.y());
        return delta;
    }

    scroll_state_snapshot.set_device_offset_for_index(node.node_id.scroll_node_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });

    Gfx::FloatPoint consumed_delta {
        new_scroll_offset.x() - old_scroll_offset.x(),
        new_scroll_offset.y() - old_scroll_offset.y()
    };
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} moved from {},{} to {},{} (consumed={},{} remaining={},{})",
        node.node_id.scroll_node_index.value(),
        old_scroll_offset.x(), old_scroll_offset.y(),
        new_scroll_offset.x(), new_scroll_offset.y(),
        consumed_delta.x(), consumed_delta.y(),
        delta.x() - consumed_delta.x(), delta.y() - consumed_delta.y());
    return {
        delta.x() - consumed_delta.x(),
        delta.y() - consumed_delta.y()
    };
}

Optional<AsyncScrollOffset> AsyncScrollTree::apply_scroll_delta(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot& scroll_state_snapshot, ScrollChaining scroll_chaining)
{
    // The compositor can advance only the scroll offsets it owns in this snapshot. Hit testing already selects an
    // ancestor when the target cannot scroll in the wheel direction at all, so once a node moves it consumes the event.
    for (size_t remaining_handoffs = m_scroll_nodes.size(); remaining_handoffs > 0 && !delta.is_zero(); --remaining_handoffs) {
        auto const* node = scroll_node_for_id(node_id);
        if (!node)
            break;

        auto remaining_delta = apply_scroll_delta_to_node(*node, delta, scroll_state_snapshot);
        if (remaining_delta != delta) {
            Compositing::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
            return AsyncScrollOffset {
                .stable_node_id = node->stable_node_id,
                .compositor_scroll_offset = scroll_offset_for_node(*node, scroll_state_snapshot),
                .unadopted_scroll_delta = delta - remaining_delta,
                .last_relative_scroll_delta = {},
                .visual_viewport_scale = {},
            };
        }

        if (scroll_chaining == ScrollChaining::None)
            break;
        auto ancestor_node_id = scrollable_ancestor_for_node(node_id, scroll_state_snapshot, delta);
        if (!ancestor_node_id.has_value())
            break;
        node_id = *ancestor_node_id;
    }

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll tree did not scroll any node for delta {},{}",
        delta.x(), delta.y());
    return {};
}

Optional<WheelScrollTarget> AsyncScrollTree::select_wheel_scroll_target(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for, ScrollChaining scroll_chaining) const
{
    // Each box is measured from where its scroll in flight is headed, so a box already headed for its extent passes the
    // first step of a gesture on to an ancestor.
    for (size_t remaining_handoffs = m_scroll_nodes.size(); remaining_handoffs > 0; --remaining_handoffs) {
        auto const* node = scroll_node_for_id(node_id);
        if (!node)
            return {};

        auto start_offset = in_flight_destination_for(node->stable_node_id).value_or(scroll_offset_for_node(*node, scroll_state_snapshot));
        Gfx::FloatPoint wheel_scrollable_delta {
            node->can_be_wheel_scrolled_horizontally ? delta.x() : 0,
            node->can_be_wheel_scrolled_vertically ? delta.y() : 0,
        };
        auto destination_offset = clamp_scroll_offset_to_node(*node, start_offset + wheel_scrollable_delta);
        if (destination_offset != start_offset) {
            return WheelScrollTarget {
                .stable_node_id = node->stable_node_id,
                .current_offset = scroll_offset_for_node(*node, scroll_state_snapshot),
                .start_offset = start_offset,
                .destination_offset = destination_offset,
            };
        }

        if (scroll_chaining == ScrollChaining::None || !node_chains_scroll_to_ancestors(*node, delta) || !node->parent_node_id.has_value())
            return {};
        node_id = *node->parent_node_id;
    }
    return {};
}

void AsyncScrollTree::rebuild_wheel_hit_test_targets(RefPtr<Compositing::DisplayList const> const& display_list, Compositing::AccumulatedVisualContextTree const* visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot)
{
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_nested_context_wheel_regions.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_cached_scroll_chaining_barriers.clear();
    m_visual_context_tree_structural_epoch.clear();
    m_scroll_state_snapshot = scroll_state_snapshot;
    if (!display_list || !visual_context_tree)
        return;

    VERIFY(display_list->compatible_visual_context_tree_structural_epoch() == visual_context_tree->structural_epoch());
    m_visual_context_tree_structural_epoch = visual_context_tree->structural_epoch();

    auto context_is_valid = [&](Compositing::ContextRef context) {
        return visual_context_tree->context_is_valid(context);
    };

    auto spatial_context_has_visual_animation = visual_context_tree->spatial_nodes_in_subtrees_of_transform_animations();
    auto viewport_rect_for_context = [&](Compositing::ContextRef context, Gfx::FloatRect const& rect) -> Optional<Gfx::FloatRect> {
        if (spatial_context_has_visual_animation[context.spatial.value()])
            return {};
        return visual_context_tree->transform_rect_to_viewport(context.spatial, rect, scroll_state_snapshot);
    };

    m_cached_wheel_hit_test_targets.ensure_capacity(m_wheel_hit_test_regions.size());
    for (auto const& target : m_wheel_hit_test_regions) {
        if (!context_is_valid(target.context))
            continue;
        m_cached_wheel_hit_test_targets.append({
            .target_node_id = target.target_node_id,
            .context = target.context,
            .rect = target.rect,
            .corner_radii = target.corner_radii,
            .viewport_rect = viewport_rect_for_context(target.context, target.rect),
            .paint_order_index = target.paint_order_index,
        });
    }

    m_cached_main_thread_wheel_event_targets.ensure_capacity(m_main_thread_wheel_event_regions.size());
    for (auto const& region : m_main_thread_wheel_event_regions) {
        if (!context_is_valid(region.context))
            continue;
        m_cached_main_thread_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = viewport_rect_for_context(region.context, region.rect),
        });
    }

    m_cached_nested_context_wheel_regions.ensure_capacity(m_nested_context_wheel_regions.size());
    for (auto const& region : m_nested_context_wheel_regions) {
        if (!context_is_valid(region.context))
            continue;
        m_cached_nested_context_wheel_regions.append({
            .region = region,
            .viewport_rect = viewport_rect_for_context(region.context, region.rect),
        });
    }

    for (auto const& region : m_blocking_wheel_event_regions) {
        // Keep invalid blockers so hit testing fails closed when their extent cannot be determined.
        m_cached_blocking_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = context_is_valid(region.context) ? viewport_rect_for_context(region.context, region.rect) : Optional<Gfx::FloatRect> {},
        });
    }

    for (auto const& barrier : m_scroll_chaining_barriers) {
        // Keep invalid barriers so hit testing fails closed when their extent cannot be determined.
        m_cached_scroll_chaining_barriers.append({
            .barrier = barrier,
            .viewport_rect = context_is_valid(barrier.context) ? viewport_rect_for_context(barrier.context, barrier.rect) : Optional<Gfx::FloatRect> {},
        });
    }
}

static bool rounded_rect_contains_point(Gfx::FloatRect const& rect, Gfx::CornerRadii const& corner_radii, Gfx::FloatPoint position_in_context)
{
    if (!rect.contains(position_in_context))
        return false;
    if (!corner_radii.has_any_radius())
        return true;
    return corner_radii.contains(position_in_context.to_type<int>(), rect.to_type<int>());
}

static bool wheel_hit_test_target_contains_point(CachedWheelHitTestTarget const& target, Gfx::FloatPoint position_in_context)
{
    return rounded_rect_contains_point(target.rect, target.corner_radii, position_in_context);
}

Optional<Gfx::FloatPoint> AsyncScrollTree::scroll_offset_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    if (auto const* node = scroll_node_for_id(node_id))
        return scroll_offset_for_node(*node, scroll_state_snapshot);
    return {};
}

Web::CSSPixelPoint AsyncScrollTree::css_pixels_from_device_offset(Gfx::FloatPoint device_offset) const
{
    return {
        Web::CSSPixels { device_offset.x() / m_device_pixels_per_css_pixel },
        Web::CSSPixels { device_offset.y() / m_device_pixels_per_css_pixel },
    };
}

Gfx::FloatPoint AsyncScrollTree::device_offset_from_css_pixels(Web::CSSPixelPoint offset) const
{
    return {
        static_cast<float>(offset.x().to_double() * m_device_pixels_per_css_pixel),
        static_cast<float>(offset.y().to_double() * m_device_pixels_per_css_pixel),
    };
}

Optional<Web::CSSPixelPoint> AsyncScrollTree::css_scroll_offset_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    return scroll_offset_for_node(node_id, scroll_state_snapshot).map([&](auto device_offset) { return css_pixels_from_device_offset(device_offset); });
}

Optional<Web::UniqueNodeID> AsyncScrollTree::document_id() const
{
    // The scroll nodes belong to the same document, whose viewport need not itself be scrollable.
    if (m_scroll_nodes.is_empty())
        return m_document_id;
    return m_scroll_nodes.first().node_id.document_id;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::viewport_scroll_node_id() const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.is_viewport)
            return node.node_id;
    }
    return {};
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scroll_node_id_for_stable_id(Web::AsyncScrollNodeStableID stable_node_id) const
{
    if (auto const* node = scroll_node_for_stable_id(stable_node_id))
        return node->node_id;
    return {};
}

bool AsyncScrollTree::blocks_wheel_event_at_position(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    if (m_has_blocking_wheel_event_region_covering_viewport || !has_wheel_hit_test_targets_for(visual_context_tree))
        return true;

    for (auto const& target : m_cached_blocking_wheel_event_targets) {
        if (!visual_context_tree.context_is_valid(target.context))
            return true;
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

bool AsyncScrollTree::main_thread_wheel_event_region_contains(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    for (auto const& target : m_cached_main_thread_wheel_event_targets) {
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!visual_context_tree.context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

Optional<AsyncScrollTree::NestedContextWheelRegionHit> AsyncScrollTree::topmost_nested_context_wheel_region_at(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    for (auto const& cached_region : m_cached_nested_context_wheel_regions.in_reverse()) {
        auto const& region = cached_region.region;
        if (cached_region.viewport_rect.has_value() && !cached_region.viewport_rect->contains(position))
            continue;
        if (!visual_context_tree.context_is_valid(region.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(region.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && rounded_rect_contains_point(region.rect, region.corner_radii, *position_in_context))
            return NestedContextWheelRegionHit { region, *position_in_context };
    }
    return {};
}

Optional<NestedContextWheelHit> AsyncScrollTree::nested_context_for_wheel_at(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    if (m_visual_context_tree_structural_epoch != visual_context_tree.structural_epoch())
        return {};
    if (main_thread_wheel_event_region_contains(visual_context_tree, position))
        return {};
    auto hit = topmost_nested_context_wheel_region_at(visual_context_tree, position);
    if (!hit.has_value() || is_covered_by_hit_test_target_painted_after(hit->region.paint_order_index, visual_context_tree, position))
        return {};
    return NestedContextWheelHit {
        .child_context_id = hit->region.child_context_id,
        .position_in_content_box = hit->position_in_context - hit->region.rect.location(),
        .content_box_size = hit->region.rect.size(),
    };
}

WheelHitTestResult AsyncScrollTree::hit_test_scroll_node_for_wheel(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta, Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> const& in_flight_destination_for, WheelStepDispatch wheel_step_dispatch, NestedContextRegions nested_context_regions) const
{
    if (m_visual_context_tree_structural_epoch != visual_context_tree.structural_epoch())
        return { .outcome = WheelHitTestOutcome::HitTestTargetsOfAnotherVisualContextTree };

    auto context_is_valid = [&](Compositing::ContextRef context) {
        return visual_context_tree.context_is_valid(context);
    };

    bool blocking_wheel_event_listeners_hold_the_step_back = wheel_step_dispatch == WheelStepDispatch::NotYetDispatched;
    if (blocking_wheel_event_listeners_hold_the_step_back && m_has_blocking_wheel_event_region_covering_viewport)
        return { .outcome = WheelHitTestOutcome::BlockedByWheelEventRegion };

    if (main_thread_wheel_event_region_contains(visual_context_tree, position))
        return { .outcome = WheelHitTestOutcome::BlockedByMainThreadRegion };
    if (nested_context_regions == NestedContextRegions::LeaveToMainThread && topmost_nested_context_wheel_region_at(visual_context_tree, position).has_value())
        return { .outcome = WheelHitTestOutcome::BlockedByMainThreadRegion };

    if (blocking_wheel_event_listeners_hold_the_step_back && blocks_wheel_event_at_position(visual_context_tree, position))
        return { .outcome = WheelHitTestOutcome::BlockedByWheelEventRegion };

    // https://drafts.csswg.org/css-overscroll-1/#scroll-chaining-and-boundary-default-actions
    // If a scroll container has no potential to scroll, because it does not overflow in the direction of the scroll,
    // the element is always considered to be at the scroll boundary.
    Vector<AsyncScrollNodeID, 2> scroll_nodes_behind_scroll_chaining_barriers;
    for (auto const& cached_barrier : m_cached_scroll_chaining_barriers) {
        auto const& barrier = cached_barrier.barrier;
        bool keeps_the_scroll = delta_is_mainly_vertical(delta) ? barrier.keeps_vertical_scrolls : barrier.keeps_horizontal_scrolls;
        if (!keeps_the_scroll)
            continue;
        if (!context_is_valid(barrier.context))
            return { .outcome = WheelHitTestOutcome::BlockedByMainThreadRegion };
        if (cached_barrier.viewport_rect.has_value() && !cached_barrier.viewport_rect->contains(position))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(barrier.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && barrier.rect.contains(*position_in_context))
            scroll_nodes_behind_scroll_chaining_barriers.append(barrier.scroll_node_id_above_barrier);
    }

    for (auto const& target : m_cached_wheel_hit_test_targets.in_reverse()) {
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (!position_in_context.has_value() || !wheel_hit_test_target_contains_point(target, *position_in_context))
            continue;
        if (!target.target_node_id.has_value())
            return { .outcome = WheelHitTestOutcome::NoScrollTarget };
        return hit_test_result_for_scroll_node(*target.target_node_id, delta, in_flight_destination_for, scroll_nodes_behind_scroll_chaining_barriers);
    }

    auto viewport_node_id = viewport_scroll_node_id();
    if (!viewport_node_id.has_value())
        return { .outcome = WheelHitTestOutcome::NoScrollTarget };
    auto const* viewport_node = scroll_node_for_id(*viewport_node_id);
    if (!viewport_node || !viewport_node->scrollport_rect.to_type<float>().contains(position))
        return { .outcome = WheelHitTestOutcome::NoScrollTarget };
    return hit_test_result_for_scroll_node(*viewport_node_id, delta, in_flight_destination_for, scroll_nodes_behind_scroll_chaining_barriers);
}

FirstWheelStepRoute AsyncScrollTree::route_first_wheel_step(WheelRoutingAdmission admission, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta, NestedContextRegions nested_context_regions) const
{
    if (admission != WheelRoutingAdmission::Accepted)
        return { input_routing_decision_for_wheel_step_not_admitted_by(admission) };
    auto scroll_target = hit_test_scroll_node_for_wheel(visual_context_tree, position, delta, {}, WheelStepDispatch::NotYetDispatched, nested_context_regions);
    FirstWheelStepRoute route { input_routing_decision_for_first_wheel_step(scroll_target.outcome) };
    if (scroll_target.node_id.has_value()) {
        if (auto const* node = scroll_node_for_id(*scroll_target.node_id))
            route.scroller = node->stable_node_id;
    }
    return route;
}

bool AsyncScrollTree::is_covered_by_hit_test_target_painted_after(u32 paint_order_index, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    if (m_visual_context_tree_structural_epoch != visual_context_tree.structural_epoch())
        return true;

    for (auto const& target : m_cached_wheel_hit_test_targets.in_reverse()) {
        if (target.paint_order_index < paint_order_index)
            break;
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!visual_context_tree.context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && wheel_hit_test_target_contains_point(target, *position_in_context))
            return true;
    }
    return false;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scroll_node_for_keyboard_scroll(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    auto const* node = scroll_node_for_stable_id(stable_node_id);
    if (!node)
        return {};

    // Keyboard routing does not depend on pointer or wheel-listener regions.
    if (can_scroll_node_by_delta(*node, scroll_state_snapshot, delta))
        return node->node_id;
    return scrollable_ancestor_for_node(node->node_id, scroll_state_snapshot, delta);
}

Gfx::FloatPoint AsyncScrollTree::clamped_scroll_offset_for_node(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return scroll_offset;
    return clamp_scroll_offset_to_node(*node, scroll_offset);
}

bool AsyncScrollTree::scroll_node_is_viewport(AsyncScrollNodeID node_id) const
{
    auto const* node = scroll_node_for_id(node_id);
    return node && node->is_viewport;
}

bool AsyncScrollTree::viewport_keeps_scrolls_along_main_axis_of(Gfx::FloatPoint delta) const
{
    return delta_is_mainly_vertical(delta) ? m_viewport_keeps_vertical_scrolls : m_viewport_keeps_horizontal_scrolls;
}

Optional<Gfx::FloatPoint> AsyncScrollTree::set_scroll_offset(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto new_scroll_offset = clamp_scroll_offset_to_node(*node, scroll_offset);
    scroll_state_snapshot.set_device_offset_for_index(node->node_id.scroll_node_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });
    Compositing::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
    return new_scroll_offset;
}

}
