/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/DisplayList.h>

#include <AK/Debug.h>

namespace Web::Compositor {

void AsyncScrollTree::set_state(AsyncScrollingState&& state)
{
    m_scroll_nodes = move(state.scroll_nodes);
    m_wheel_hit_test_regions = move(state.wheel_hit_test_targets);
    m_main_thread_wheel_event_regions = move(state.main_thread_wheel_event_regions);
    m_blocking_wheel_event_regions = move(state.blocking_wheel_event_regions);
    m_has_blocking_wheel_event_region_covering_viewport = state.has_blocking_wheel_event_region_covering_viewport;
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_version.clear();
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_id(AsyncScrollNodeID node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.node_id == node_id)
            return &node;
    }
    return nullptr;
}

WheelHitTestResult AsyncScrollTree::hit_test_result_for_scroll_node(AsyncScrollNodeID node_id, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};
    if (can_scroll_node_by_delta(*node, m_scroll_state_snapshot, delta))
        return { node_id, false };
    if (auto ancestor = scrollable_ancestor_for_node(node_id, m_scroll_state_snapshot, delta); ancestor.has_value())
        return { ancestor, false };
    return {};
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_stable_id(AsyncScrollNodeStableID stable_node_id) const
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

Gfx::FloatPoint AsyncScrollTree::scroll_offset_for_node(AsyncScrollNode const& node, Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    auto device_offset = scroll_state_snapshot.device_offset_for_index(node.node_id.scroll_node_index);
    return { -device_offset.x(), -device_offset.y() };
}

bool AsyncScrollTree::can_scroll_node_by_delta(AsyncScrollNode const& node, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta)
{
    auto scroll_offset = scroll_offset_for_node(node, scroll_state_snapshot);
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

bool AsyncScrollTree::has_non_zero_scroll_delta(Gfx::FloatPoint delta)
{
    return delta.x() != 0 || delta.y() != 0;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scrollable_ancestor_for_node(AsyncScrollNodeID node_id, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto parent_node_id = node->parent_node_id;
    while (parent_node_id.has_value()) {
        auto const* parent_node = scroll_node_for_id(*parent_node_id);
        if (!parent_node)
            return {};
        if (can_scroll_node_by_delta(*parent_node, scroll_state_snapshot, delta))
            return *parent_node_id;
        parent_node_id = parent_node->parent_node_id;
    }
    return {};
}

Gfx::FloatPoint AsyncScrollTree::apply_scroll_delta_to_node(AsyncScrollNode const& node, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot& scroll_state_snapshot)
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

static void set_or_append_scroll_offset(Vector<AsyncScrollOffset>& scroll_offsets, AsyncScrollNode const& node, Gfx::FloatPoint compositor_scroll_offset, Gfx::FloatPoint unadopted_scroll_delta)
{
    for (auto& existing : scroll_offsets) {
        if (existing.stable_node_id == node.stable_node_id) {
            existing.compositor_scroll_offset = compositor_scroll_offset;
            existing.unadopted_scroll_delta.translate_by(unadopted_scroll_delta);
            return;
        }
    }
    scroll_offsets.append({
        .stable_node_id = node.stable_node_id,
        .compositor_scroll_offset = compositor_scroll_offset,
        .unadopted_scroll_delta = unadopted_scroll_delta,
    });
}

Vector<AsyncScrollOffset> AsyncScrollTree::apply_scroll_delta(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Painting::AccumulatedVisualContextTree const& visual_context_tree, Painting::ScrollStateSnapshot& scroll_state_snapshot)
{
    // The compositor can advance only the scroll offsets it owns in this snapshot. Hit testing already selects an
    // ancestor when the target cannot scroll in the wheel direction at all, so once a node moves it consumes the event.
    Vector<AsyncScrollOffset> scroll_offsets;
    auto remaining_delta = delta;
    for (size_t remaining_handoffs = m_scroll_nodes.size(); remaining_handoffs > 0 && has_non_zero_scroll_delta(remaining_delta); --remaining_handoffs) {
        auto const* node = scroll_node_for_id(node_id);
        if (!node)
            break;

        auto delta_before_scroll = remaining_delta;
        remaining_delta = apply_scroll_delta_to_node(*node, remaining_delta, scroll_state_snapshot);
        if (remaining_delta != delta_before_scroll) {
            Gfx::FloatPoint consumed_delta {
                delta_before_scroll.x() - remaining_delta.x(),
                delta_before_scroll.y() - remaining_delta.y(),
            };
            set_or_append_scroll_offset(scroll_offsets, *node, scroll_offset_for_node(*node, scroll_state_snapshot), consumed_delta);
            break;
        }

        auto ancestor_node_id = scrollable_ancestor_for_node(node_id, scroll_state_snapshot, remaining_delta);
        if (!ancestor_node_id.has_value())
            break;
        node_id = *ancestor_node_id;
    }

    if (!scroll_offsets.is_empty())
        Painting::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
    else
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll tree did not scroll any node for delta {},{}",
            delta.x(), delta.y());

    return scroll_offsets;
}

void AsyncScrollTree::rebuild_wheel_hit_test_targets(RefPtr<Painting::DisplayList const> const& display_list, Painting::AccumulatedVisualContextTree const* visual_context_tree, Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_version.clear();
    m_scroll_state_snapshot = scroll_state_snapshot;
    if (!display_list || !visual_context_tree)
        return;

    VERIFY(display_list->compatible_visual_context_tree_version() == visual_context_tree->version());
    m_visual_context_tree_version = visual_context_tree->version();

    auto context_is_valid = [&](Painting::ContextRef context) {
        return context.spatial.value() < visual_context_tree->spatial_nodes().size()
            && (context.frame == Painting::NO_FRAME_NODE || context.frame.value() < visual_context_tree->frame_nodes().size());
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
            .viewport_rect = visual_context_tree->transform_rect_to_viewport(target.context.spatial, target.rect, scroll_state_snapshot),
        });
    }

    m_cached_main_thread_wheel_event_targets.ensure_capacity(m_main_thread_wheel_event_regions.size());
    for (auto const& region : m_main_thread_wheel_event_regions) {
        if (!context_is_valid(region.context))
            continue;
        m_cached_main_thread_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = visual_context_tree->transform_rect_to_viewport(region.context.spatial, region.rect, scroll_state_snapshot),
        });
    }

    for (auto const& region : m_blocking_wheel_event_regions) {
        if (!context_is_valid(region.context))
            continue;
        m_cached_blocking_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = visual_context_tree->transform_rect_to_viewport(region.context.spatial, region.rect, scroll_state_snapshot),
        });
    }
}

void AsyncScrollTree::clear_wheel_hit_test_targets()
{
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_version.clear();
}

static bool wheel_hit_test_target_contains_point(CachedWheelHitTestTarget const& target, Gfx::FloatPoint position_in_context)
{
    if (!target.rect.contains(position_in_context))
        return false;
    if (!target.corner_radii.has_any_radius())
        return true;
    return target.corner_radii.contains(position_in_context.to_type<int>(), target.rect.to_type<int>());
}

Optional<Gfx::FloatPoint> AsyncScrollTree::scroll_offset_for_node(AsyncScrollNodeID node_id, Painting::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    if (auto const* node = scroll_node_for_id(node_id))
        return scroll_offset_for_node(*node, scroll_state_snapshot);
    return {};
}

Optional<AsyncScrollNodeID> AsyncScrollTree::viewport_scroll_node_id() const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.is_viewport)
            return node.node_id;
    }
    return {};
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scroll_node_id_for_stable_id(AsyncScrollNodeStableID stable_node_id) const
{
    if (auto const* node = scroll_node_for_stable_id(stable_node_id))
        return node->node_id;
    return {};
}

WheelHitTestResult AsyncScrollTree::hit_test_scroll_node_for_wheel(Painting::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta, SnapContainerHandling snap_container_handling) const
{
    auto scrolled_on_the_main_thread_instead = [&](WheelHitTestResult const& result) {
        if (snap_container_handling == SnapContainerHandling::ScrollOnCompositor || !result.node_id.has_value())
            return false;
        auto const* node = scroll_node_for_id(*result.node_id);
        if (!node)
            return false;
        return (node->snaps_scroll_position_horizontally && delta.x() != 0)
            || (node->snaps_scroll_position_vertically && delta.y() != 0);
    };
    auto hit_test_result_for_wheel_scroll_of_node = [&](AsyncScrollNodeID node_id) {
        auto result = hit_test_result_for_scroll_node(node_id, delta);
        if (scrolled_on_the_main_thread_instead(result))
            return WheelHitTestResult { {}, true };
        return result;
    };

    if (m_visual_context_tree_version != visual_context_tree.version())
        return {};

    auto context_is_valid = [&](Painting::ContextRef context) {
        return context.spatial.value() < visual_context_tree.spatial_nodes().size()
            && (context.frame == Painting::NO_FRAME_NODE || context.frame.value() < visual_context_tree.frame_nodes().size());
    };

    if (m_has_blocking_wheel_event_region_covering_viewport)
        return { {}, false, true };

    for (auto const& target : m_cached_main_thread_wheel_event_targets) {
        if (!target.viewport_rect.contains(position))
            continue;

        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return { {}, true };
    }

    for (auto const& target : m_cached_blocking_wheel_event_targets) {
        if (!target.viewport_rect.contains(position))
            continue;

        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return { {}, false, true };
    }

    for (auto const& target : m_cached_wheel_hit_test_targets.in_reverse()) {
        if (!target.viewport_rect.contains(position))
            continue;

        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (!position_in_context.has_value() || !wheel_hit_test_target_contains_point(target, *position_in_context))
            continue;
        if (!target.target_node_id.has_value())
            return {};
        return hit_test_result_for_wheel_scroll_of_node(*target.target_node_id);
    }

    auto viewport_node_id = viewport_scroll_node_id();
    if (!viewport_node_id.has_value())
        return {};
    auto const* viewport_node = scroll_node_for_id(*viewport_node_id);
    if (!viewport_node || !viewport_node->scrollport_rect.to_type<float>().contains(position))
        return {};
    return hit_test_result_for_wheel_scroll_of_node(*viewport_node_id);
}

bool AsyncScrollTree::scroll_node_is_viewport(AsyncScrollNodeID node_id) const
{
    auto const* node = scroll_node_for_id(node_id);
    return node && node->is_viewport;
}

Optional<Gfx::FloatPoint> AsyncScrollTree::set_scroll_offset(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset, Painting::AccumulatedVisualContextTree const& visual_context_tree, Painting::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto new_scroll_offset = clamp_scroll_offset_to_node(*node, scroll_offset);
    scroll_state_snapshot.set_device_offset_for_index(node->node_id.scroll_node_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });
    Painting::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
    return new_scroll_offset;
}

}
