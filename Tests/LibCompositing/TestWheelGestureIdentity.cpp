/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Scrolling/WheelGestureIdentity.h>
#include <LibCompositing/Types.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>

using namespace Compositing;

static constexpr Gfx::FloatPoint position { 100, 100 };
static constexpr float position_slop = wheel_gesture_position_slop_in_css_pixels;
static auto const rest_longer_than_the_idle_delay = user_scroll_settle_delay + AK::Duration::from_milliseconds(100);

TEST_CASE(fingers_resting_on_the_touchpad_end_the_gesture_after_the_idle_delay)
{
    auto start = MonotonicTime::now_coarse();
    auto gesture = WheelGestureIdentity::started_by(position, Web::ScrollGesturePhase::Ongoing, 0, start);
    EXPECT(gesture.is_held_by_fingers());

    auto after_rest = start + rest_longer_than_the_idle_delay;
    EXPECT(!gesture.is_continued_by(position, Web::ScrollGesturePhase::Ongoing, 0, after_rest, position_slop, TouchpadGestureEnd::AfterIdleDelay));
}

TEST_CASE(fingers_resting_on_the_touchpad_keep_the_gesture_until_they_lift)
{
    auto start = MonotonicTime::now_coarse();
    auto gesture = WheelGestureIdentity::started_by(position, Web::ScrollGesturePhase::Ongoing, 0, start);

    auto after_rest = start + rest_longer_than_the_idle_delay;
    EXPECT(gesture.is_continued_by(position, Web::ScrollGesturePhase::Ongoing, 0, after_rest, position_slop, TouchpadGestureEnd::WhenFingersLift));

    // The fingers lift, and fingers that touch down long after start another gesture.
    gesture.advance_to(Web::ScrollGesturePhase::Ended, after_rest);
    EXPECT(!gesture.is_held_by_fingers());
    auto much_later = after_rest + rest_longer_than_the_idle_delay;
    EXPECT(!gesture.is_continued_by(position, Web::ScrollGesturePhase::Ongoing, 0, much_later, position_slop, TouchpadGestureEnd::WhenFingersLift));
}

TEST_CASE(momentum_that_pauses_ends_the_gesture_after_the_idle_delay)
{
    auto start = MonotonicTime::now_coarse();
    auto gesture = WheelGestureIdentity::started_by(position, Web::ScrollGesturePhase::Ongoing, 0, start);
    gesture.advance_to(Web::ScrollGesturePhase::Momentum, start);
    EXPECT(!gesture.is_held_by_fingers());

    auto after_pause = start + rest_longer_than_the_idle_delay;
    EXPECT(!gesture.is_continued_by(position, Web::ScrollGesturePhase::Momentum, 0, after_pause, position_slop, TouchpadGestureEnd::WhenFingersLift));
}

TEST_CASE(a_mouse_wheel_that_pauses_ends_the_gesture_after_the_idle_delay)
{
    auto start = MonotonicTime::now_coarse();
    auto gesture = WheelGestureIdentity::started_by(position, Web::ScrollGesturePhase::None, 0, start);
    EXPECT(!gesture.is_held_by_fingers());

    auto after_pause = start + rest_longer_than_the_idle_delay;
    EXPECT(!gesture.is_continued_by(position, Web::ScrollGesturePhase::None, 0, after_pause, position_slop, TouchpadGestureEnd::WhenFingersLift));
}
