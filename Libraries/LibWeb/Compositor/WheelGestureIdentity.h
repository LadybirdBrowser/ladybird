/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibGfx/Point.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Compositor {

inline constexpr AK::Duration wheel_gesture_momentum_grace_after_end = AK::Duration::from_milliseconds(100);
inline constexpr float wheel_gesture_position_slop_in_css_pixels = 10;

// A wheel gesture keeps scrolling the scroller its first event was routed to, so the compositor and the main thread
// both have to tell whether a wheel event continues the gesture they latched or starts another one. Both decide by
// the rules here, so that they agree on the scroller an event belongs to without telling each other.
class WEB_API WheelGestureIdentity {
public:
    static WheelGestureIdentity started_by(Gfx::FloatPoint position, ScrollGesturePhase, u32 modifiers, MonotonicTime now);

    // Positions and the slop are in whatever unit the caller routes in.
    bool is_continued_by(Gfx::FloatPoint position, ScrollGesturePhase, u32 modifiers, MonotonicTime now, float position_slop) const;
    void advance_to(ScrollGesturePhase, MonotonicTime now);

private:
    enum class Kind : u8 {
        PhaseLess,
        Phased,
    };

    WheelGestureIdentity(Kind, Gfx::FloatPoint first_position, u32 modifiers, MonotonicTime now);

    static Kind kind_for(ScrollGesturePhase);

    Kind m_kind;
    Gfx::FloatPoint m_first_position;
    u32 m_modifiers { 0 };
    MonotonicTime m_last_event_time;
    Optional<MonotonicTime> m_ended_at;
};

}
