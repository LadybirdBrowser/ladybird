/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <Compositor/WheelGestureRouting.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>

namespace Compositor {

static u64 allocate_wheel_gesture_id()
{
    static u64 last_wheel_gesture_id = 0;
    return ++last_wheel_gesture_id;
}

static float position_slop_in_device_pixels(Compositing::AsyncScrollTree const& async_scroll_tree)
{
    return Compositing::wheel_gesture_position_slop_in_css_pixels * static_cast<float>(async_scroll_tree.device_pixels_per_css_pixel());
}

void WheelGestureRouting::did_install_scrolling_state(Compositing::AsyncScrollingState const& async_scrolling_state)
{
    m_admission = Compositing::wheel_routing_admission_for(async_scrolling_state, m_wheel_event_listener_state_generation);
    m_wheel_event_listener_state_generation = max(m_wheel_event_listener_state_generation, async_scrolling_state.wheel_event_listener_state_generation);
    m_has_blocking_wheel_event_listeners = async_scrolling_state.has_blocking_wheel_event_listeners;
}

void WheelGestureRouting::invalidate_wheel_event_listener_state(u64 generation)
{
    m_wheel_event_listener_state_generation = max(m_wheel_event_listener_state_generation, generation);
    m_admission = Compositing::WheelRoutingAdmission::StaleWheelEventListeners;
    m_has_blocking_wheel_event_listeners = true;
}

bool WheelGestureRouting::admits_async_pinch() const
{
    // Pinch zoom scales the visual viewport, which a page whose viewport cannot be scrolled has too.
    return first_is_one_of(m_admission, Compositing::WheelRoutingAdmission::Accepted, Compositing::WheelRoutingAdmission::NoScrollNode)
        && !m_has_blocking_wheel_event_listeners;
}

WheelGestureRouting::NotedWheelEvent WheelGestureRouting::note_wheel_event(Web::MouseEvent const& event, Gfx::FloatPoint position, Compositing::AsyncScrollTree const& async_scroll_tree, MonotonicTime now)
{
    auto phase = event.scroll_gesture_phase;
    if (event.id != 0) {
        if (phase == Web::ScrollGesturePhase::Ongoing)
            m_newest_wheel_event_ids_from_ui.ongoing = event.id;
        else if (phase == Web::ScrollGesturePhase::Momentum)
            m_newest_wheel_event_ids_from_ui.momentum = event.id;
        else if (phase == Web::ScrollGesturePhase::Ended)
            m_newest_wheel_event_ids_from_ui.ended = event.id;
    }

    bool starts_gesture = !m_gesture.has_value() || !m_gesture->identity.is_continued_by(position, phase, event.modifiers, now, position_slop_in_device_pixels(async_scroll_tree), m_touchpad_gesture_end);
    if (starts_gesture) {
        m_gesture = Gesture {
            .id = allocate_wheel_gesture_id(),
            .identity = Compositing::WheelGestureIdentity::started_by(position, phase, event.modifiers, now),
        };
        m_gesture_had_wheel_event_with_delta = false;
        m_latch.clear();
    } else {
        m_gesture->identity.advance_to(phase, now);
    }
    bool is_first_wheel_event_with_delta = !m_gesture_had_wheel_event_with_delta && (event.wheel_delta_x != 0 || event.wheel_delta_y != 0);
    if (is_first_wheel_event_with_delta)
        m_gesture_had_wheel_event_with_delta = true;
    return { .starts_gesture = starts_gesture, .is_first_wheel_event_with_delta = is_first_wheel_event_with_delta };
}

Optional<u64> WheelGestureRouting::gesture_id() const
{
    if (!m_gesture.has_value())
        return {};
    return m_gesture->id;
}

Optional<Web::CompositorContextId> WheelGestureRouting::wheel_event_listener_context() const
{
    if (!m_gesture.has_value())
        return {};
    return m_gesture->wheel_event_listener_context;
}

void WheelGestureRouting::set_wheel_event_listener_context(Web::CompositorContextId context_id)
{
    if (m_gesture.has_value())
        m_gesture->wheel_event_listener_context = context_id;
}

void WheelGestureRouting::note_delta_left_to_the_page(Gfx::FloatPoint delta_in_css_pixels)
{
    if (m_gesture.has_value())
        m_gesture->delta_left_to_the_page += { AK::abs(delta_in_css_pixels.x()), AK::abs(delta_in_css_pixels.y()) };
}

bool WheelGestureRouting::take_delta_left_to_the_page(Gfx::FloatPoint delta_in_css_pixels)
{
    if (!m_gesture.has_value())
        return false;
    // The page sends back the deltas it was given, which may have been summed up on the way.
    constexpr float rounding_slack = 0.01f;
    auto& delta_left = m_gesture->delta_left_to_the_page;
    Gfx::FloatPoint step { AK::abs(delta_in_css_pixels.x()), AK::abs(delta_in_css_pixels.y()) };
    if (step.x() > delta_left.x() + rounding_slack || step.y() > delta_left.y() + rounding_slack)
        return false;
    delta_left = { max(0.0f, delta_left.x() - step.x()), max(0.0f, delta_left.y() - step.y()) };
    return true;
}

void WheelGestureRouting::forget_gesture()
{
    m_gesture.clear();
    m_gesture_had_wheel_event_with_delta = false;
    m_latch.clear();
}

bool WheelGestureRouting::step_was_overtaken_by_ui(Web::ScrollGesturePhase phase, u64 input_event_id) const
{
    auto const& newest = m_newest_wheel_event_ids_from_ui;
    switch (phase) {
    case Web::ScrollGesturePhase::None:
        return false;
    case Web::ScrollGesturePhase::Ongoing:
        // The fingers lifted after this step.
        return newest.momentum > input_event_id || newest.ended > input_event_id;
    case Web::ScrollGesturePhase::Momentum:
        // The momentum ended, or the fingers touched down again, after this step.
        return newest.ended > input_event_id || newest.ongoing > input_event_id;
    case Web::ScrollGesturePhase::Ended:
        // Another gesture started after this end.
        return newest.ongoing > input_event_id;
    }
    VERIFY_NOT_REACHED();
}

WheelGestureRouting::ResolvedLatch WheelGestureRouting::resolve_latch(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::AccumulatedVisualContextTree const& current_visual_context_tree, Web::ScrollGesturePhase scroll_gesture_phase, Optional<u64> gesture_id)
{
    if (!m_latch.has_value())
        return {};
    if (m_latch->gesture_id != gesture_id) {
        m_latch.clear();
        return {};
    }
    if (m_latch->nested_context_id.has_value() || !m_latch->stable_node_id.has_value()) {
        m_latch->last_scroll_gesture_phase = scroll_gesture_phase;
        return { .nested_context_id = m_latch->nested_context_id, .keeps_steps = !m_latch->nested_context_id.has_value() };
    }
    auto scroll_node_index = async_scroll_tree.scroll_node_index_for_stable_id(*m_latch->stable_node_id);
    if (!scroll_node_index.has_value()) {
        m_latch.clear();
        return {};
    }
    // The hit test a latched step skips would refuse a tree the wheel hit-test targets were not built for.
    if (!async_scroll_tree.has_wheel_hit_test_targets_for(current_visual_context_tree))
        return {};
    m_latch->last_scroll_gesture_phase = scroll_gesture_phase;
    return { .scroll_node_index = scroll_node_index };
}

Optional<Web::AsyncScrollNodeStableID> WheelGestureRouting::latched_scroller() const
{
    if (!m_latch.has_value())
        return {};
    return m_latch->stable_node_id;
}

bool WheelGestureRouting::gesture_lasts_until_fingers_lift() const
{
    return m_touchpad_gesture_end == Compositing::TouchpadGestureEnd::WhenFingersLift && m_gesture.has_value() && m_gesture->identity.is_held_by_fingers();
}

Optional<Web::AsyncScrollNodeStableID> WheelGestureRouting::scroller_held_by_fingers() const
{
    if (m_touchpad_gesture_end != Compositing::TouchpadGestureEnd::WhenFingersLift || !m_latch.has_value())
        return {};
    // The gesture here has every event; a context nested in another has only the steps handed to it, on its latch.
    bool held_by_fingers = m_gesture.has_value() ? m_gesture->identity.is_held_by_fingers() : m_latch->last_scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing;
    if (!held_by_fingers)
        return {};
    return m_latch->stable_node_id;
}

void WheelGestureRouting::latch(Web::AsyncScrollNodeStableID stable_node_id, Web::ScrollGesturePhase scroll_gesture_phase, Optional<u64> gesture_id)
{
    m_latch = Latch {
        .gesture_id = gesture_id,
        .stable_node_id = stable_node_id,
        .last_scroll_gesture_phase = scroll_gesture_phase,
    };
}

void WheelGestureRouting::latch_to_nested_context(Web::CompositorContextId nested_context_id, Optional<Web::AsyncScrollNodeStableID> scroller, Web::ScrollGesturePhase scroll_gesture_phase, Optional<u64> gesture_id)
{
    m_latch = Latch {
        .gesture_id = gesture_id,
        .stable_node_id = scroller,
        .last_scroll_gesture_phase = scroll_gesture_phase,
        .nested_context_id = nested_context_id,
    };
}

void WheelGestureRouting::latch_keeping_steps(Web::ScrollGesturePhase scroll_gesture_phase, Optional<u64> gesture_id)
{
    m_latch = Latch {
        .gesture_id = gesture_id,
        .stable_node_id = {},
        .last_scroll_gesture_phase = scroll_gesture_phase,
    };
}

void WheelGestureRouting::relatch_to(Web::AsyncScrollNodeStableID stable_node_id)
{
    if (m_latch.has_value() && m_latch->stable_node_id.has_value() && !m_latch->nested_context_id.has_value())
        m_latch->stable_node_id = stable_node_id;
}

Optional<Web::CompositorContextId> WheelGestureRouting::nested_context_of_latch() const
{
    if (!m_latch.has_value())
        return {};
    return m_latch->nested_context_id;
}

void WheelGestureRouting::clear_latch_of_gesture(u64 gesture_id)
{
    if (has_latch_for_gesture(gesture_id))
        m_latch.clear();
}

void WheelGestureRouting::note_page_let_gesture_through(u64 gesture_id)
{
    m_gesture_the_page_let_through = GestureThePageLetThrough {
        .gesture_id = gesture_id,
        .wheel_event_listener_state_generation = m_wheel_event_listener_state_generation,
    };
}

void WheelGestureRouting::forget_page_let_gesture_through(u64 gesture_id)
{
    if (m_gesture_the_page_let_through.has_value() && m_gesture_the_page_let_through->gesture_id == gesture_id)
        m_gesture_the_page_let_through.clear();
}

bool WheelGestureRouting::page_let_gesture_through(Optional<u64> gesture_id) const
{
    return gesture_id.has_value()
        && m_gesture_the_page_let_through.has_value()
        && m_gesture_the_page_let_through->gesture_id == *gesture_id
        && m_gesture_the_page_let_through->wheel_event_listener_state_generation == m_wheel_event_listener_state_generation;
}

Optional<Web::InputRoutingDecision> WheelGestureRouting::reason_to_hold_latched_steps_back(Optional<u64> gesture_id) const
{
    if (page_let_gesture_through(gesture_id))
        return {};
    // Blocking listeners over all of the viewport hold every step back, and so do listeners that a context without a
    // current display list cannot place.
    if (first_is_one_of(m_admission, Compositing::WheelRoutingAdmission::Accepted, Compositing::WheelRoutingAdmission::NoScrollNode))
        return {};
    return Compositing::input_routing_decision_for_wheel_step_not_admitted_by(m_admission);
}

bool WheelGestureRouting::momentum_of_latched_gesture_can_spring_scroller_back() const
{
    return m_latch.has_value() && !m_latch->momentum_has_sprung_the_scroller_back;
}

void WheelGestureRouting::set_momentum_of_latched_gesture_has_sprung_scroller_back(bool has_sprung_back)
{
    if (m_latch.has_value())
        m_latch->momentum_has_sprung_the_scroller_back = has_sprung_back;
}

}
