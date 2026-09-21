/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Compositor/WheelGestureIdentity.h>
#include <LibWeb/Page/InputEvent.h>

namespace Web::Compositor {

WheelGestureIdentity::WheelGestureIdentity(Kind kind, Gfx::FloatPoint first_position, u32 modifiers, MonotonicTime now)
    : m_kind(kind)
    , m_first_position(first_position)
    , m_modifiers(modifiers)
    , m_last_event_time(now)
{
}

WheelGestureIdentity::Kind WheelGestureIdentity::kind_for(ScrollGesturePhase phase)
{
    return phase == ScrollGesturePhase::None ? Kind::PhaseLess : Kind::Phased;
}

WheelGestureIdentity WheelGestureIdentity::started_by(Gfx::FloatPoint position, ScrollGesturePhase phase, u32 modifiers, MonotonicTime now)
{
    WheelGestureIdentity gesture { kind_for(phase), position, modifiers, now };
    gesture.advance_to(phase, now);
    return gesture;
}

bool WheelGestureIdentity::is_continued_by(Gfx::FloatPoint position, ScrollGesturePhase phase, u32 modifiers, MonotonicTime now, float position_slop) const
{
    if (now - m_last_event_time > user_scroll_settle_delay)
        return false;
    if (kind_for(phase) != m_kind)
        return false;
    if (m_kind == Kind::PhaseLess)
        return modifiers == m_modifiers && position.distance_from(m_first_position) <= position_slop;
    if (!m_ended_at.has_value())
        return true;
    bool phase_may_follow_an_ended_gesture = phase == ScrollGesturePhase::Momentum || phase == ScrollGesturePhase::Ended;
    return phase_may_follow_an_ended_gesture && now - *m_ended_at <= wheel_gesture_momentum_grace_after_end;
}

void WheelGestureIdentity::advance_to(ScrollGesturePhase phase, MonotonicTime now)
{
    m_last_event_time = now;
    if (phase != ScrollGesturePhase::Ended) {
        m_ended_at.clear();
        return;
    }
    if (!m_ended_at.has_value())
        m_ended_at = now;
}

}
