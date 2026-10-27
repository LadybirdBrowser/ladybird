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

bool WheelGestureRouting::only_blocking_wheel_event_listeners_hold_steps_back() const
{
    return first_is_one_of(m_admission, Compositing::WheelRoutingAdmission::BlockingWheelEventListeners, Compositing::WheelRoutingAdmission::StaleWheelEventListeners);
}

bool WheelGestureRouting::note_wheel_event_from_ui(Web::MouseEvent const& event, Gfx::FloatPoint position, Compositing::AsyncScrollTree const& async_scroll_tree, MonotonicTime now)
{
    return note_wheel_event(event.id, event.scroll_gesture_phase, event.modifiers, { event.wheel_delta_x, event.wheel_delta_y }, position, async_scroll_tree, now);
}

bool WheelGestureRouting::note_wheel_event(u64 input_event_id, Web::ScrollGesturePhase phase, u32 modifiers, Gfx::FloatPoint wheel_delta, Gfx::FloatPoint position, Compositing::AsyncScrollTree const& async_scroll_tree, MonotonicTime now)
{
    if (input_event_id != 0) {
        if (phase == Web::ScrollGesturePhase::Ongoing)
            m_newest_wheel_event_ids_from_ui.ongoing = input_event_id;
        else if (phase == Web::ScrollGesturePhase::Momentum)
            m_newest_wheel_event_ids_from_ui.momentum = input_event_id;
        else if (phase == Web::ScrollGesturePhase::Ended)
            m_newest_wheel_event_ids_from_ui.ended = input_event_id;
    }

    bool starts_gesture = !m_gesture.has_value() || !m_gesture->is_continued_by(position, phase, modifiers, now, position_slop_in_device_pixels(async_scroll_tree), m_touchpad_gesture_end);
    if (starts_gesture) {
        m_gesture = Compositing::WheelGestureIdentity::started_by(position, phase, modifiers, now);
        m_gesture_had_wheel_event_with_delta = false;
        m_latch.clear();
    } else {
        m_gesture->advance_to(phase, now);
    }
    if (!m_gesture_had_wheel_event_with_delta && !wheel_delta.is_zero()) {
        m_gesture_had_wheel_event_with_delta = true;
        m_id_of_first_wheel_event_with_delta = input_event_id;
    }
    return starts_gesture;
}

void WheelGestureRouting::forget_gesture()
{
    m_gesture.clear();
    m_gesture_had_wheel_event_with_delta = false;
    m_id_of_first_wheel_event_with_delta = 0;
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

WheelGestureRouting::ResolvedLatch WheelGestureRouting::resolve_latch(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::AccumulatedVisualContextTree const& current_visual_context_tree, Web::ScrollGesturePhase scroll_gesture_phase)
{
    if (!m_latch.has_value())
        return {};
    if (m_latch->nested_context_id.has_value() || !m_latch->stable_node_id.has_value()) {
        m_latch->last_scroll_gesture_phase = scroll_gesture_phase;
        return { .nested_context_id = m_latch->nested_context_id, .keeps_steps = !m_latch->nested_context_id.has_value() };
    }
    auto node_id = async_scroll_tree.scroll_node_id_for_stable_id(*m_latch->stable_node_id);
    if (!node_id.has_value()) {
        m_latch.clear();
        return {};
    }
    // The hit test a latched step skips would refuse a tree the wheel hit-test targets were not built for.
    if (!async_scroll_tree.has_wheel_hit_test_targets_for(current_visual_context_tree))
        return {};
    m_latch->last_scroll_gesture_phase = scroll_gesture_phase;
    return { .node_id = node_id };
}

Optional<Web::AsyncScrollNodeStableID> WheelGestureRouting::latched_scroller() const
{
    if (!m_latch.has_value())
        return {};
    return m_latch->stable_node_id;
}

bool WheelGestureRouting::gesture_lasts_until_fingers_lift() const
{
    return m_touchpad_gesture_end == Compositing::TouchpadGestureEnd::WhenFingersLift && m_gesture.has_value() && m_gesture->is_held_by_fingers();
}

Optional<Web::AsyncScrollNodeStableID> WheelGestureRouting::scroller_held_by_fingers() const
{
    if (m_touchpad_gesture_end != Compositing::TouchpadGestureEnd::WhenFingersLift || !m_latch.has_value())
        return {};
    // The gesture here has every event; a context nested in another has only the steps handed to it, on its latch.
    bool held_by_fingers = m_gesture.has_value() ? m_gesture->is_held_by_fingers() : m_latch->last_scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing;
    if (!held_by_fingers)
        return {};
    return m_latch->stable_node_id;
}

void WheelGestureRouting::latch(Web::AsyncScrollNodeStableID stable_node_id, Web::ScrollGesturePhase scroll_gesture_phase)
{
    m_latch = Latch {
        .stable_node_id = stable_node_id,
        .last_scroll_gesture_phase = scroll_gesture_phase,
    };
}

void WheelGestureRouting::latch_to_nested_context(Web::CompositorContextId nested_context_id, Optional<Web::AsyncScrollNodeStableID> scroller, Web::ScrollGesturePhase scroll_gesture_phase)
{
    m_latch = Latch {
        .stable_node_id = scroller,
        .last_scroll_gesture_phase = scroll_gesture_phase,
        .nested_context_id = nested_context_id,
    };
}

void WheelGestureRouting::latch_keeping_steps(Web::ScrollGesturePhase scroll_gesture_phase)
{
    m_latch = Latch {
        .stable_node_id = {},
        .last_scroll_gesture_phase = scroll_gesture_phase,
    };
}

void WheelGestureRouting::relatch_to(Web::AsyncScrollNodeStableID stable_node_id)
{
    if (m_latch.has_value() && m_latch->stable_node_id.has_value() && !m_latch->nested_context_id.has_value())
        m_latch->stable_node_id = stable_node_id;
}

void WheelGestureRouting::note_page_let_latched_gesture_through()
{
    VERIFY(m_latch.has_value());
    m_latch->wheel_event_listener_state_generation_the_page_let_the_gesture_through_at = m_wheel_event_listener_state_generation;
}

bool WheelGestureRouting::page_let_latched_touchpad_gesture_through(Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase) const
{
    if (!m_latch.has_value())
        return false;
    if (wheel_delta_precision != Web::WheelDeltaPrecision::Precise || scroll_gesture_phase == Web::ScrollGesturePhase::None)
        return false;
    return m_latch->wheel_event_listener_state_generation_the_page_let_the_gesture_through_at == m_wheel_event_listener_state_generation;
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
