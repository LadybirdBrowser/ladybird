/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ScrollSnapController.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Compositor {

static Web::CSSPixelPoint css_pixels_from_device_offset(Gfx::FloatPoint device_offset, double device_pixels_per_css_pixel)
{
    return {
        Web::CSSPixels { device_offset.x() / device_pixels_per_css_pixel },
        Web::CSSPixels { device_offset.y() / device_pixels_per_css_pixel },
    };
}

void ScrollSnapController::did_install_scrolling_state(Web::Compositor::AsyncScrollTree const& scroll_tree)
{
    m_candidates.clear();
    m_nodes.remove_all_matching([&](auto const& stable_node_id, auto const&) {
        return !scroll_tree.scroll_node_id_for_stable_id(stable_node_id).has_value();
    });
}

void ScrollSnapController::did_start_main_thread_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id)
{
    // A scroll started for any reason other than this controller's input is going somewhere the gesture never asked
    // for, so the gesture's steps then travel from the scrolling box itself.
    m_nodes.remove(stable_node_id);
}

void ScrollSnapController::did_start_snap_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncScrollOperationID operation_id, Web::CSSPixelPoint destination)
{
    auto& state = node_state(stable_node_id);
    state.snap_scroll_operation_id = operation_id;
    state.snap_scroll_destination = destination;
    state.expected_scroll_offset = destination;
}

void ScrollSnapController::did_end_snap_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncScrollOperationID operation_id, Optional<Web::CSSPixelPoint> scroll_offset)
{
    auto state = m_nodes.get(stable_node_id);
    if (!state.has_value() || state->snap_scroll_operation_id != operation_id)
        return;
    state->snap_scroll_operation_id = {};
    state->snap_scroll_destination = {};
    state->expected_scroll_offset = scroll_offset;
}

bool ScrollSnapController::is_snap_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncScrollOperationID operation_id) const
{
    auto state = m_nodes.get(stable_node_id);
    return state.has_value() && state->snap_scroll_operation_id == operation_id;
}

ScrollSnapController::NodeState& ScrollSnapController::node_state(Web::Compositor::AsyncScrollNodeStableID stable_node_id)
{
    return m_nodes.ensure(stable_node_id);
}

Web::Compositor::SnapAxisCandidates const& ScrollSnapController::candidates_for(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncSnapContainer const& snap_container)
{
    return m_candidates.ensure(stable_node_id, [&] {
        return Web::Compositor::build_snap_candidates(snap_container.geometry, snap_container.areas, snap_container.geometry.axes);
    });
}

static Web::CSSPixelPoint clamp_scroll_offset(Web::CSSPixelPoint offset, Web::Compositor::SnapContainerGeometry const& geometry)
{
    return {
        clamp(offset.x(), geometry.min_scroll_offset.x(), geometry.max_scroll_offset.x()),
        clamp(offset.y(), geometry.min_scroll_offset.y(), geometry.max_scroll_offset.y()),
    };
}

ScrollSnapController::WheelStepDecision ScrollSnapController::decide_discrete_step(Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Web::Compositor::AsyncScrollNodeID node_id, Web::CSSPixelPoint delta, MonotonicTime now)
{
    WheelStepDecision decision;

    auto const* node = scroll_tree.scroll_node(node_id);
    auto const* snap_container = scroll_tree.snap_container_for_node(node_id);
    if (!node || !snap_container)
        return decision;
    auto const& geometry = snap_container->geometry;

    // The step selects a snap position only along the axes it travels in that the container snaps in.
    if (!(geometry.axes.x && delta.x() != 0) && !(geometry.axes.y && delta.y() != 0))
        return decision;

    auto current_device_offset = scroll_tree.scroll_offset_for_node(node_id, scroll_state_snapshot);
    if (!current_device_offset.has_value())
        return decision;
    auto current_scroll_offset = css_pixels_from_device_offset(*current_device_offset, scroll_tree.device_pixels_per_css_pixel());

    decision.stable_node_id = node->stable_node_id;
    auto& state = node_state(node->stable_node_id);

    // A gesture ends once its steps stop arriving, and once anything but this controller has moved the scrolling box.
    bool gesture_ended = (state.gesture_input_deadline.has_value() && now > *state.gesture_input_deadline)
        || (!state.snap_scroll_operation_id.has_value() && state.expected_scroll_offset != current_scroll_offset);
    if (gesture_ended)
        state.unsnapped_scroll_destination = {};

    // A step selects its snap position from the offset the gesture's input deltas have reached rather than from the
    // snap position it is scrolling to.
    auto step_start = state.unsnapped_scroll_destination.value_or(current_scroll_offset);
    auto unsnapped_destination = clamp_scroll_offset(step_start + delta, geometry);

    // NB: A step with only an intended direction ignores every snap position up to the offset its input asked for.
    Web::Compositor::SnapSelectionStrategy strategy {
        .type = Web::Compositor::SnapSelectionStrategy::Type::Direction,
        .start_offset = step_start,
        .displacement = delta,
        .starting_positions_boundary = unsnapped_destination,
    };
    auto evaluated_axes = Web::Compositor::axes_to_evaluate(geometry.axes, strategy);
    auto selection = Web::Compositor::select_snap_destination(geometry, candidates_for(node->stable_node_id, *snap_container), unsnapped_destination, strategy, evaluated_axes);

    // The step travels only along axes the container selects no snap position in, so it is left to the ordinary
    // relative scroll.
    if (!(selection.snapped_x && delta.x() != 0) && !(selection.snapped_y && delta.y() != 0))
        return decision;

    state.unsnapped_scroll_destination = unsnapped_destination;
    state.gesture_input_deadline = now + Web::Compositor::user_scroll_settle_delay;

    decision.initial_scroll_offset = current_scroll_offset;
    decision.unsnapped_scroll_destination = unsnapped_destination;
    decision.selection = move(selection);

    // A step whose selected snap position is where the scrolling box already rests, or is already scrolling to, is
    // consumed without disturbing where it is going.
    auto in_flight_destination = state.snap_scroll_operation_id.has_value() ? state.snap_scroll_destination : Optional<Web::CSSPixelPoint> {};
    if (decision.selection.position == in_flight_destination.value_or(current_scroll_offset)) {
        if (!state.snap_scroll_operation_id.has_value())
            state.expected_scroll_offset = current_scroll_offset;
        decision.outcome = WheelStepOutcome::Consumed;
        return decision;
    }

    decision.outcome = WheelStepOutcome::StartSnapScroll;
    return decision;
}

}
