/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCompositing/Scrolling/ScrollFling.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositor {

// Synthesizes the momentum of a touchpad flick as wheel events, for platforms that send no momentum scroll events.
class ScrollFlingController {
public:
    void note_wheel_event(Web::MouseEvent const&, MonotonicTime now);
    // Starts a fling if the gesture that the event ends was a flick.
    bool start_if_flicked(Web::MouseEvent const& gesture_end_event, MonotonicTime now);
    bool is_active() const { return m_active_fling.has_value(); }

    // The next momentum step of the fling, or the end of its gesture once the fling has run out or was told to end.
    Optional<Web::MouseEvent> take_step(MonotonicTime frame_time);
    void end_on_next_step();
    // The end of the gesture of the fling, if one is active.
    Optional<Web::MouseEvent> end();
    // Ends the fling before a wheel event from a new gesture or other input.
    Optional<Web::MouseEvent> interrupt(Web::MouseEvent const&, MonotonicTime now);

private:
    struct ActiveFling {
        Compositing::ScrollFling fling;
        // The fling steps use the position and modifiers of the gesture end event.
        Web::DevicePixelPoint position;
        Web::DevicePixelPoint screen_position;
        Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
        bool ends_on_next_step { false };
    };

    Web::MouseEvent event_of_active_fling(Gfx::FloatPoint delta, Web::ScrollGesturePhase) const;

    Compositing::ScrollVelocityTracker m_velocity_tracker;
    Compositing::ScrollFlingBooster m_booster;
    Optional<ActiveFling> m_active_fling;
};

}
