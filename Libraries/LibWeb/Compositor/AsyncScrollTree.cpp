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
    m_sticky_areas = move(state.sticky_areas);
    m_main_thread_wheel_event_regions = move(state.main_thread_wheel_event_regions);
    m_viewport_rect = state.viewport_rect;
    m_main_thread_wheel_event_targets.clear();
    m_wheel_scroll_targets.clear();
}

AsyncScrollNode* AsyncScrollTree::scroll_node_for_id(AsyncScrollNodeID node_id)
{
    for (auto& node : m_scroll_nodes) {
        if (node.node_id == node_id)
            return &node;
    }
    return nullptr;
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_id(AsyncScrollNodeID node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.node_id == node_id)
            return &node;
    }
    return nullptr;
}

AsyncStickyArea const* AsyncScrollTree::sticky_area_for_scroll_frame_index(Painting::ScrollFrameIndex scroll_frame_index) const
{
    for (auto const& sticky_area : m_sticky_areas) {
        if (sticky_area.scroll_frame_index == scroll_frame_index)
            return &sticky_area;
    }
    return nullptr;
}

Gfx::FloatPoint AsyncScrollTree::clamp_scroll_offset_to_node(AsyncScrollNode const& node, Gfx::FloatPoint scroll_offset)
{
    scroll_offset.set_x(max(0.0f, min(scroll_offset.x(), node.max_scroll_offset.x())));
    scroll_offset.set_y(max(0.0f, min(scroll_offset.y(), node.max_scroll_offset.y())));
    return scroll_offset;
}

bool AsyncScrollTree::can_scroll_node_by_delta(AsyncScrollNode const& node, Gfx::FloatPoint delta)
{
    if (delta.x() < 0 && node.scroll_offset.x() > 0)
        return true;
    if (delta.x() > 0 && node.scroll_offset.x() < node.max_scroll_offset.x())
        return true;
    if (delta.y() < 0 && node.scroll_offset.y() > 0)
        return true;
    if (delta.y() > 0 && node.scroll_offset.y() < node.max_scroll_offset.y())
        return true;
    return false;
}

bool AsyncScrollTree::can_scroll_target_by_delta(WheelScrollTarget const& target, Gfx::FloatPoint delta)
{
    if (delta.x() < 0 && target.can_scroll_left)
        return true;
    if (delta.x() > 0 && target.can_scroll_right)
        return true;
    if (delta.y() < 0 && target.can_scroll_up)
        return true;
    if (delta.y() > 0 && target.can_scroll_down)
        return true;
    return false;
}

bool AsyncScrollTree::has_non_zero_scroll_delta(Gfx::FloatPoint delta)
{
    return delta.x() != 0 || delta.y() != 0;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scrollable_ancestor_for_node(AsyncScrollNodeID node_id, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto parent_node_id = node->parent_node_id;
    while (parent_node_id.has_value()) {
        auto const* parent_node = scroll_node_for_id(*parent_node_id);
        if (!parent_node)
            return {};
        if (can_scroll_node_by_delta(*parent_node, delta))
            return *parent_node_id;
        parent_node_id = parent_node->parent_node_id;
    }
    return {};
}

Optional<Painting::ScrollFrameIndex> AsyncScrollTree::parent_scroll_frame_index(Painting::ScrollFrameIndex scroll_frame_index) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.node_id.scroll_frame_index != scroll_frame_index)
            continue;
        if (!node.parent_node_id.has_value())
            return {};
        return node.parent_node_id->scroll_frame_index;
    }

    if (auto const* sticky_area = sticky_area_for_scroll_frame_index(scroll_frame_index))
        return sticky_area->parent_scroll_frame_index;

    return {};
}

Gfx::FloatPoint AsyncScrollTree::cumulative_device_offset_for_frame(Painting::ScrollFrameIndex scroll_frame_index, Painting::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    Gfx::FloatPoint offset;
    for (auto index = scroll_frame_index; index.value();) {
        offset.translate_by(scroll_state_snapshot.device_offset_for_index(index));
        auto parent_index = parent_scroll_frame_index(index);
        if (!parent_index.has_value())
            break;
        index = *parent_index;
    }
    return offset;
}

Gfx::FloatPoint AsyncScrollTree::apply_scroll_delta_to_node(AsyncScrollNode& node, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto old_scroll_offset = node.scroll_offset;
    auto new_scroll_offset = clamp_scroll_offset_to_node(node, old_scroll_offset.translated(delta));
    if (new_scroll_offset == old_scroll_offset) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} did not move for delta {},{} (offset={},{} max={},{})",
            node.node_id.scroll_frame_index.value(), delta.x(), delta.y(), old_scroll_offset.x(), old_scroll_offset.y(), node.max_scroll_offset.x(), node.max_scroll_offset.y());
        return delta;
    }

    node.scroll_offset = new_scroll_offset;
    scroll_state_snapshot.set_device_offset_for_index(node.node_id.scroll_frame_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });

    Gfx::FloatPoint consumed_delta {
        new_scroll_offset.x() - old_scroll_offset.x(),
        new_scroll_offset.y() - old_scroll_offset.y()
    };
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} moved from {},{} to {},{} (consumed={},{} remaining={},{})",
        node.node_id.scroll_frame_index.value(),
        old_scroll_offset.x(), old_scroll_offset.y(),
        new_scroll_offset.x(), new_scroll_offset.y(),
        consumed_delta.x(), consumed_delta.y(),
        delta.x() - consumed_delta.x(), delta.y() - consumed_delta.y());
    return {
        delta.x() - consumed_delta.x(),
        delta.y() - consumed_delta.y()
    };
}

void AsyncScrollTree::update_sticky_offsets(Painting::ScrollStateSnapshot& scroll_state_snapshot) const
{
    // This mirrors ViewportPaintable::refresh_scroll_state(), but consumes the compositor's mutated
    // ScrollStateSnapshot instead of live layout objects.
    for (auto const& sticky_area : m_sticky_areas) {
        if (!sticky_area.nearest_scrolling_ancestor_index.value())
            continue;

        Gfx::FloatPoint parent_sticky_offset;
        if (sticky_area.parent_scroll_frame_index.value() && sticky_area_for_scroll_frame_index(sticky_area.parent_scroll_frame_index))
            parent_sticky_offset = cumulative_device_offset_for_frame(sticky_area.parent_scroll_frame_index, scroll_state_snapshot);

        auto sticky_position_in_ancestor = sticky_area.position_relative_to_scroll_ancestor.translated(parent_sticky_offset);

        auto containing_block_region = sticky_area.containing_block_region;
        if (sticky_area.needs_parent_offset_adjustment)
            containing_block_region.translate_by(parent_sticky_offset);

        auto min_offset_within_containing_block = containing_block_region.top_left();
        Gfx::FloatPoint max_offset_within_containing_block {
            containing_block_region.right() - sticky_area.border_box_size.width(),
            containing_block_region.bottom() - sticky_area.border_box_size.height()
        };

        auto ancestor_device_offset = scroll_state_snapshot.device_offset_for_index(sticky_area.nearest_scrolling_ancestor_index);
        Gfx::FloatPoint scroll_ancestor_scroll_offset { -ancestor_device_offset.x(), -ancestor_device_offset.y() };
        Gfx::FloatRect scrollport_rect { scroll_ancestor_scroll_offset, sticky_area.scrollport_size };
        Gfx::FloatPoint sticky_offset;

        if (sticky_area.inset_top.has_value()) {
            if (scrollport_rect.top() > sticky_position_in_ancestor.y() - *sticky_area.inset_top)
                sticky_offset.set_y(min(scrollport_rect.top() + *sticky_area.inset_top, max_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_area.inset_left.has_value()) {
            if (scrollport_rect.left() > sticky_position_in_ancestor.x() - *sticky_area.inset_left)
                sticky_offset.set_x(min(scrollport_rect.left() + *sticky_area.inset_left, max_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }
        if (sticky_area.inset_bottom.has_value()) {
            if (scrollport_rect.bottom() < sticky_position_in_ancestor.y() + sticky_area.border_box_size.height() + *sticky_area.inset_bottom)
                sticky_offset.set_y(max(scrollport_rect.bottom() - sticky_area.border_box_size.height() - *sticky_area.inset_bottom, min_offset_within_containing_block.y()) - sticky_position_in_ancestor.y());
        }
        if (sticky_area.inset_right.has_value()) {
            if (scrollport_rect.right() < sticky_position_in_ancestor.x() + sticky_area.border_box_size.width() + *sticky_area.inset_right)
                sticky_offset.set_x(max(scrollport_rect.right() - sticky_area.border_box_size.width() - *sticky_area.inset_right, min_offset_within_containing_block.x()) - sticky_position_in_ancestor.x());
        }

        scroll_state_snapshot.set_device_offset_for_index(sticky_area.scroll_frame_index, sticky_offset);
    }
}

bool AsyncScrollTree::apply_scroll_delta(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Painting::ScrollStateSnapshot& scroll_state_snapshot)
{
    // The compositor can advance only the scroll offsets it owns in this snapshot. When a target reaches an edge,
    // the remaining wheel delta is handed to the nearest scrollable ancestor from the same immutable tree.
    bool scrolled = false;
    auto remaining_delta = delta;
    for (size_t remaining_handoffs = m_scroll_nodes.size(); remaining_handoffs > 0 && has_non_zero_scroll_delta(remaining_delta); --remaining_handoffs) {
        auto* node = scroll_node_for_id(node_id);
        if (!node)
            break;

        auto delta_before_scroll = remaining_delta;
        remaining_delta = apply_scroll_delta_to_node(*node, remaining_delta, scroll_state_snapshot);
        if (remaining_delta != delta_before_scroll)
            scrolled = true;
        if (!has_non_zero_scroll_delta(remaining_delta))
            break;

        auto ancestor_node_id = scrollable_ancestor_for_node(node_id, remaining_delta);
        if (!ancestor_node_id.has_value())
            break;
        node_id = *ancestor_node_id;
    }

    if (scrolled)
        update_sticky_offsets(scroll_state_snapshot);
    else
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll tree did not scroll any node for delta {},{}",
            delta.x(), delta.y());

    return scrolled;
}

void AsyncScrollTree::rebuild_wheel_scroll_targets(RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    m_wheel_scroll_targets.clear();
    m_main_thread_wheel_event_targets.clear();
    if (!display_list)
        return;

    auto const& visual_context_tree = display_list->visual_context_tree();
    for (auto const& node : m_scroll_nodes) {
        auto viewport_rect = node.is_viewport
            ? Gfx::FloatRect { {}, m_viewport_rect.size().to_type<float>() }
            : visual_context_tree.transform_rect_to_viewport(node.hit_test_visual_context_index, node.scrollport_rect.to_type<float>(), scroll_state_snapshot);

        m_wheel_scroll_targets.append({
            .node_id = node.node_id,
            .viewport_rect = viewport_rect,
            .can_scroll_left = node.scroll_offset.x() > 0,
            .can_scroll_right = node.scroll_offset.x() < node.max_scroll_offset.x(),
            .can_scroll_up = node.scroll_offset.y() > 0,
            .can_scroll_down = node.scroll_offset.y() < node.max_scroll_offset.y(),
        });
    }

    for (auto const& region : m_main_thread_wheel_event_regions) {
        m_main_thread_wheel_event_targets.append({
            .viewport_rect = visual_context_tree.transform_rect_to_viewport(region.visual_context_index, region.rect, scroll_state_snapshot),
        });
    }
}

void AsyncScrollTree::clear_wheel_scroll_targets()
{
    m_wheel_scroll_targets.clear();
    m_main_thread_wheel_event_targets.clear();
}

Optional<Gfx::FloatPoint> AsyncScrollTree::scroll_offset_for_node(AsyncScrollNodeID node_id) const
{
    if (auto const* node = scroll_node_for_id(node_id))
        return node->scroll_offset;
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

Optional<AsyncScrollNodeID> AsyncScrollTree::hit_test_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const
{
    for (auto const& target : m_main_thread_wheel_event_targets) {
        if (target.viewport_rect.contains(position))
            return {};
    }

    Optional<AsyncScrollNodeID> scroll_target;
    for (auto const& target : m_wheel_scroll_targets) {
        if (!target.viewport_rect.contains(position))
            continue;
        if (!can_scroll_target_by_delta(target, delta))
            continue;
        scroll_target = target.node_id;
    }
    return scroll_target;
}

bool AsyncScrollTree::scroll_node_is_viewport(AsyncScrollNodeID node_id) const
{
    auto const* node = scroll_node_for_id(node_id);
    return node && node->is_viewport;
}

Optional<Gfx::FloatPoint> AsyncScrollTree::set_scroll_offset(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset, Painting::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    node->scroll_offset = clamp_scroll_offset_to_node(*node, scroll_offset);
    scroll_state_snapshot.set_device_offset_for_index(node->node_id.scroll_frame_index, { -node->scroll_offset.x(), -node->scroll_offset.y() });
    update_sticky_offsets(scroll_state_snapshot);
    return node->scroll_offset;
}

}
