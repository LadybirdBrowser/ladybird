/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Scrolling/WheelGestureIdentity.h>
#include <LibCompositing/Types.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositing {

WheelGestureIdentity::WheelGestureIdentity(Kind kind, Gfx::FloatPoint first_position, u32 modifiers, MonotonicTime now)
    : m_kind(kind)
    , m_first_position(first_position)
    , m_modifiers(modifiers)
    , m_last_event_time(now)
{
}

WheelGestureIdentity::Kind WheelGestureIdentity::kind_for(Web::ScrollGesturePhase phase)
{
    return phase == Web::ScrollGesturePhase::None ? Kind::PhaseLess : Kind::Phased;
}

WheelGestureIdentity WheelGestureIdentity::started_by(Gfx::FloatPoint position, Web::ScrollGesturePhase phase, u32 modifiers, MonotonicTime now)
{
    WheelGestureIdentity gesture { kind_for(phase), position, modifiers, now };
    gesture.advance_to(phase, now);
    return gesture;
}

bool WheelGestureIdentity::is_held_by_fingers() const
{
    return m_kind == Kind::Phased && !m_ended_at.has_value() && !m_momentum_has_started;
}

bool WheelGestureIdentity::is_continued_by(Gfx::FloatPoint position, Web::ScrollGesturePhase phase, u32 modifiers, MonotonicTime now, float position_slop, TouchpadGestureEnd touchpad_gesture_end) const
{
    bool lasts_while_fingers_rest = touchpad_gesture_end == TouchpadGestureEnd::WhenFingersLift && is_held_by_fingers();
    if (!lasts_while_fingers_rest && now - m_last_event_time > user_scroll_settle_delay)
        return false;
    if (kind_for(phase) != m_kind)
        return false;
    if (m_kind == Kind::PhaseLess)
        return modifiers == m_modifiers && position.distance_from(m_first_position) <= position_slop;
    // Fingers that touch down while the momentum of the gesture runs start another gesture, whether or not the end of
    // that momentum arrived first.
    if (phase == Web::ScrollGesturePhase::Ongoing && m_momentum_has_started)
        return false;
    if (!m_ended_at.has_value())
        return true;
    bool phase_may_follow_an_ended_gesture = phase == Web::ScrollGesturePhase::Momentum || phase == Web::ScrollGesturePhase::Ended;
    return phase_may_follow_an_ended_gesture && now - *m_ended_at <= wheel_gesture_momentum_grace_after_end;
}

void WheelGestureIdentity::advance_to(Web::ScrollGesturePhase phase, MonotonicTime now)
{
    m_last_event_time = now;
    if (phase == Web::ScrollGesturePhase::Momentum)
        m_momentum_has_started = true;
    if (phase != Web::ScrollGesturePhase::Ended) {
        m_ended_at.clear();
        return;
    }
    if (!m_ended_at.has_value())
        m_ended_at = now;
}

void WheelGestureIdentity::did_handle_event(MonotonicTime now)
{
    bool handled_event_ended_gesture = m_ended_at == m_last_event_time;
    if (handled_event_ended_gesture)
        m_ended_at = now;
    m_last_event_time = now;
}

}
