/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ScrollSnapController.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Compositor {

void ScrollSnapController::did_install_scrolling_state(Web::Compositor::AsyncScrollTree const& scroll_tree)
{
    m_candidates.clear();
    m_nodes.remove_all_matching([&](auto const& stable_node_id, auto const&) {
        return !scroll_tree.scroll_node_id_for_stable_id(stable_node_id).has_value();
    });
}

void ScrollSnapController::note_gesture_phase(Web::ScrollGesturePhase phase)
{
    switch (phase) {
    case Web::ScrollGesturePhase::None:
        end_gesture();
        break;
    case Web::ScrollGesturePhase::Ongoing:
        reset_momentum_fling_state();
        break;
    case Web::ScrollGesturePhase::Momentum:
    case Web::ScrollGesturePhase::Ended:
        break;
    }
}

void ScrollSnapController::reset_momentum_fling_state()
{
    m_momentum_snap_position_selection = MomentumSnapPositionSelection::NotSelectedYet;
    m_momentum_fling_estimator.reset();
}

void ScrollSnapController::end_gesture()
{
    reset_momentum_fling_state();
    for (auto& [stable_node_id, state] : m_nodes)
        state.gesture_start = {};
}

void ScrollSnapController::did_scroll_node_plainly(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::ScrollGesturePhase phase, Web::CSSPixelPoint scroll_offset_before_scroll)
{
    if (phase != Web::ScrollGesturePhase::Ongoing && phase != Web::ScrollGesturePhase::Momentum)
        return;
    auto& state = node_state(stable_node_id);
    if (!state.gesture_start.has_value())
        state.gesture_start = GestureStart { .offset = scroll_offset_before_scroll };
    if (phase == Web::ScrollGesturePhase::Momentum)
        state.gesture_start->travels_under_momentum = true;
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
    state.snap_scroll = InFlightSnapScroll { .operation_id = operation_id, .destination = destination };
    state.expected_scroll_offset = destination;
}

void ScrollSnapController::did_end_snap_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncScrollOperationID operation_id, Optional<Web::CSSPixelPoint> scroll_offset)
{
    if (!is_snap_scroll(stable_node_id, operation_id))
        return;
    auto& state = node_state(stable_node_id);
    state.snap_scroll = {};
    state.expected_scroll_offset = scroll_offset;
}

bool ScrollSnapController::is_snap_scroll(Web::Compositor::AsyncScrollNodeStableID stable_node_id, Web::Compositor::AsyncScrollOperationID operation_id) const
{
    auto state = m_nodes.get(stable_node_id);
    return state.has_value() && state->snap_scroll.has_value() && state->snap_scroll->operation_id == operation_id;
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

Optional<ScrollSnapController::SnapTarget> ScrollSnapController::snap_target_for(Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Web::Compositor::AsyncScrollNodeID node_id)
{
    auto const* node = scroll_tree.scroll_node_for_id(node_id);
    auto const* snap_container = scroll_tree.snap_container_for_node(node_id);
    auto current_scroll_offset = scroll_tree.css_scroll_offset_for_node(node_id, scroll_state_snapshot);
    if (!node || !snap_container || !current_scroll_offset.has_value())
        return {};
    return SnapTarget { .stable_node_id = node->stable_node_id, .snap_container = snap_container, .current_scroll_offset = *current_scroll_offset };
}

static Web::CSSPixelPoint clamp_scroll_offset(Web::CSSPixelPoint offset, Web::Compositor::SnapContainerGeometry const& geometry)
{
    return {
        clamp(offset.x(), geometry.min_scroll_offset.x(), geometry.max_scroll_offset.x()),
        clamp(offset.y(), geometry.min_scroll_offset.y(), geometry.max_scroll_offset.y()),
    };
}

// A scroll travels only along axes the container selects no snap position in is left to the ordinary relative scroll.
static bool selected_snap_position_along_travel(Web::Compositor::SnapDestination const& selection, Web::CSSPixelPoint displacement)
{
    return (selection.snapped_x && displacement.x() != 0) || (selection.snapped_y && displacement.y() != 0);
}

Optional<ScrollSnapController::WheelStepDecision> ScrollSnapController::decide_discrete_step(Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Web::Compositor::AsyncScrollNodeID node_id, Web::CSSPixelPoint delta, MonotonicTime now)
{
    auto target = snap_target_for(scroll_tree, scroll_state_snapshot, node_id);
    if (!target.has_value())
        return {};
    auto const& geometry = target->snap_container->geometry;

    // The step selects a snap position only along the axes it travels in that the container snaps in.
    if (!(geometry.axes.x && delta.x() != 0) && !(geometry.axes.y && delta.y() != 0))
        return {};

    auto current_scroll_offset = target->current_scroll_offset;
    auto& state = node_state(target->stable_node_id);

    // A gesture ends once its steps stop arriving, and once anything but this controller has moved the scrolling box.
    bool gesture_ended = (state.gesture_input_deadline.has_value() && now > *state.gesture_input_deadline)
        || (!state.snap_scroll.has_value() && state.expected_scroll_offset != current_scroll_offset);
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
    auto selection = Web::Compositor::select_snap_destination(geometry, candidates_for(target->stable_node_id, *target->snap_container), unsnapped_destination, strategy, evaluated_axes);
    if (!selected_snap_position_along_travel(selection, delta))
        return {};

    state.unsnapped_scroll_destination = unsnapped_destination;
    state.gesture_input_deadline = now + Web::Compositor::user_scroll_settle_delay;

    auto in_flight_destination = state.snap_scroll.map([](auto const& snap_scroll) { return snap_scroll.destination; });
    if (selection.position == in_flight_destination.value_or(current_scroll_offset)) {
        if (!state.snap_scroll.has_value())
            state.expected_scroll_offset = current_scroll_offset;
        return WheelStepDecision { StepConsumed {} };
    }

    return WheelStepDecision { SnapScrollStart {
        .stable_node_id = target->stable_node_id,
        .initial_scroll_offset = current_scroll_offset,
        .unsnapped_scroll_destination = unsnapped_destination,
        .selection = move(selection),
        .animation_kind = Web::Compositor::ScrollAnimationKind::SmoothScroll,
    } };
}

Optional<ScrollSnapController::WheelStepDecision> ScrollSnapController::decide_momentum_delta(Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Web::Compositor::AsyncScrollNodeID node_id, Web::CSSPixelPoint momentum_delta)
{
    if (m_momentum_snap_position_selection == MomentumSnapPositionSelection::ScrollingToSelectedPosition)
        return WheelStepDecision { StepConsumed {} };
    if (m_momentum_snap_position_selection == MomentumSnapPositionSelection::NoPositionSelected)
        return {};

    auto remaining_displacement = m_momentum_fling_estimator.estimate_remaining_displacement(momentum_delta);
    if (!remaining_displacement.has_value())
        return {};

    auto target = snap_target_for(scroll_tree, scroll_state_snapshot, node_id);
    if (!target.has_value()) {
        m_momentum_snap_position_selection = MomentumSnapPositionSelection::NoPositionSelected;
        return {};
    }
    auto const& geometry = target->snap_container->geometry;
    auto& state = node_state(target->stable_node_id);

    // The momentum travels on from the snap position a snap scroll already in flight is headed for.
    auto in_flight_destination = state.snap_scroll.map([](auto const& snap_scroll) { return snap_scroll.destination; });
    auto step_start = in_flight_destination.value_or(target->current_scroll_offset);
    auto unsnapped_destination = clamp_scroll_offset(step_start + *remaining_displacement, geometry);

    Web::Compositor::SnapSelectionStrategy strategy {
        .type = Web::Compositor::SnapSelectionStrategy::Type::EndPositionAndDirection,
        .start_offset = step_start,
        .displacement = *remaining_displacement,
        .starting_positions_boundary = {},
    };
    auto evaluated_axes = Web::Compositor::axes_to_evaluate(geometry.axes, strategy);
    auto selection = Web::Compositor::select_snap_destination(geometry, candidates_for(target->stable_node_id, *target->snap_container), unsnapped_destination, strategy, evaluated_axes);
    if (!selected_snap_position_along_travel(selection, *remaining_displacement)) {
        m_momentum_snap_position_selection = MomentumSnapPositionSelection::NoPositionSelected;
        return {};
    }

    m_momentum_snap_position_selection = MomentumSnapPositionSelection::ScrollingToSelectedPosition;
    if (selection.position == step_start)
        return WheelStepDecision { StepConsumed {} };
    return WheelStepDecision { SnapScrollStart {
        .stable_node_id = target->stable_node_id,
        .initial_scroll_offset = target->current_scroll_offset,
        .unsnapped_scroll_destination = unsnapped_destination,
        .selection = move(selection),
        .animation_kind = Web::Compositor::ScrollAnimationKind::Momentum,
    } };
}

Vector<ScrollSnapController::GestureEndSnap> ScrollSnapController::decide_gesture_end(Web::Compositor::AsyncScrollTree const& scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    Vector<GestureEndSnap> snaps;
    for (auto& [stable_node_id, state] : m_nodes) {
        if (!state.gesture_start.has_value())
            continue;
        // Momentum that selected a snap position is still scrolling to it.
        if (state.snap_scroll.has_value())
            continue;

        auto node_id = scroll_tree.scroll_node_id_for_stable_id(stable_node_id);
        if (!node_id.has_value())
            continue;
        auto target = snap_target_for(scroll_tree, scroll_state_snapshot, *node_id);
        if (!target.has_value())
            continue;
        auto const& geometry = target->snap_container->geometry;
        auto current_scroll_offset = target->current_scroll_offset;

        // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
        // A pan reports where the user's input came to rest, so it settles as an absolute scroll; the momentum a flick
        // traveled under must not have passed a snap position that always stops on the way from where it started.
        Web::Compositor::SnapSelectionStrategy strategy {
            .type = Web::Compositor::SnapSelectionStrategy::Type::EndPosition,
            .start_offset = state.gesture_start->travels_under_momentum ? Optional<Web::CSSPixelPoint> { state.gesture_start->offset } : Optional<Web::CSSPixelPoint> {},
            .displacement = current_scroll_offset - state.gesture_start->offset,
            .starting_positions_boundary = {},
        };
        auto evaluated_axes = Web::Compositor::axes_to_evaluate(geometry.axes, strategy);
        if (evaluated_axes.is_empty())
            continue;
        auto selection = Web::Compositor::select_snap_destination(geometry, candidates_for(stable_node_id, *target->snap_container), current_scroll_offset, strategy, evaluated_axes);
        if (selection.position == current_scroll_offset)
            continue;

        snaps.append({
            .node_id = *node_id,
            .snap_scroll = {
                .stable_node_id = stable_node_id,
                .initial_scroll_offset = current_scroll_offset,
                .unsnapped_scroll_destination = current_scroll_offset,
                .selection = move(selection),
                .animation_kind = Web::Compositor::ScrollAnimationKind::SmoothScroll,
            },
        });
    }
    end_gesture();
    return snaps;
}

}
