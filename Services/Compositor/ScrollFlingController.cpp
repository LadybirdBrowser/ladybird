/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ScrollFlingController.h>

namespace Compositor {

static bool is_touchpad_step(Web::MouseEvent const& event)
{
    return event.wheel_delta_precision == Web::WheelDeltaPrecision::Precise && event.scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing;
}

void ScrollFlingController::note_wheel_event(Web::MouseEvent const& event, MonotonicTime now)
{
    if (is_touchpad_step(event)) {
        m_velocity_tracker.add_step({ static_cast<float>(event.wheel_delta_x), static_cast<float>(event.wheel_delta_y) }, now);
        return;
    }
    m_velocity_tracker.reset();
    // Other wheel input cancels the boost of the next fling.
    if (event.scroll_gesture_phase != Web::ScrollGesturePhase::Ended)
        m_booster.reset();
}

bool ScrollFlingController::start_if_flicked(Web::MouseEvent const& gesture_end_event, MonotonicTime now)
{
    VERIFY(gesture_end_event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended);
    auto velocity = m_velocity_tracker.velocity_at_lift(now);
    m_velocity_tracker.reset();
    if (!velocity.has_value())
        return false;

    auto fling = Compositing::ScrollFling::start(m_booster.boosted_velocity(*velocity, now), now);
    if (!fling.has_value())
        return false;
    m_active_fling = ActiveFling {
        .fling = fling.release_value(),
        .position = gesture_end_event.position,
        .screen_position = gesture_end_event.screen_position,
        .modifiers = gesture_end_event.modifiers,
    };
    return true;
}

Optional<Web::MouseEvent> ScrollFlingController::take_step(MonotonicTime frame_time)
{
    if (!m_active_fling.has_value())
        return {};
    if (m_active_fling->ends_on_next_step)
        return end();

    auto delta = m_active_fling->fling.step(frame_time);
    if (!delta.has_value())
        return end();
    if (delta->is_zero())
        return {};
    return event_of_active_fling(*delta, Web::ScrollGesturePhase::Momentum);
}

void ScrollFlingController::end_on_next_step()
{
    VERIFY(m_active_fling.has_value());
    m_active_fling->ends_on_next_step = true;
}

Optional<Web::MouseEvent> ScrollFlingController::end()
{
    if (!m_active_fling.has_value())
        return {};

    // WebContent holds the gesture open while momentum events arrive. The end of the gesture releases it.
    auto event = event_of_active_fling({}, Web::ScrollGesturePhase::Ended);
    m_active_fling.clear();
    return event;
}

Optional<Web::MouseEvent> ScrollFlingController::interrupt(Web::MouseEvent const& event, MonotonicTime now)
{
    if (!m_active_fling.has_value())
        return {};

    // A new touchpad step stops the fling. The next flick can add the velocity of this fling.
    if (is_touchpad_step(event))
        m_booster.fling_was_interrupted(m_active_fling->fling.velocity_at(now), now);
    return end();
}

Web::MouseEvent ScrollFlingController::event_of_active_fling(Gfx::FloatPoint delta, Web::ScrollGesturePhase phase) const
{
    VERIFY(m_active_fling.has_value());
    return {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = m_active_fling->position,
        .screen_position = m_active_fling->screen_position,
        .modifiers = m_active_fling->modifiers,
        .wheel_delta_x = delta.x(),
        .wheel_delta_y = delta.y(),
        .wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(delta.x(), Web::WheelDeltaPrecision::Precise),
        .wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(delta.y(), Web::WheelDeltaPrecision::Precise),
        .wheel_delta_precision = Web::WheelDeltaPrecision::Precise,
        .scroll_gesture_phase = phase,
        .click_count = 0,
        .browser_data = nullptr,
        .async_scroll_performed_default_action = false,
        // The IDs of UI input events start at 1. Thus, ID 0 does not match a UI event.
        .id = 0,
    };
}

}
