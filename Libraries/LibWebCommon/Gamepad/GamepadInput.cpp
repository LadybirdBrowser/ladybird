/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWebCommon/Gamepad/GamepadInput.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dfn-button-press-threshold
// For buttons which do not have a digital switch to indicate a pure pressed or released state, the user
// agent MUST choose a button press threshold to indicate the button as pressed when its value is above a
// certain amount. If the platform API gives a recommended value, the user agent SHOULD use that. In other
// cases, the user agent SHOULD choose some other reasonable value.
static constexpr double ANALOG_BUTTON_PRESS_THRESHOLD = 0.1;

static constexpr double AXIS_GESTURE_THRESHOLD = 0.5;

// https://w3c.github.io/gamepad/#dfn-map-and-normalize-axes
double normalize_axis_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    return 2.0 * static_cast<double>(logical_value - logical_minimum) / static_cast<double>(logical_maximum - logical_minimum) - 1.0;
}

// https://w3c.github.io/gamepad/#dfn-map-and-normalize-buttons
double normalize_button_value(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    return static_cast<double>(logical_value - logical_minimum) / static_cast<double>(logical_maximum - logical_minimum);
}

// https://w3c.github.io/gamepad/#dfn-map-and-normalize-buttons
bool is_button_pressed(i16 logical_value, i16 logical_minimum, i16 logical_maximum)
{
    // NB: Buttons with a logical maximum of 1 have a digital switch.
    if (logical_maximum == 1)
        return logical_value == 1;
    return normalize_button_value(logical_value, logical_minimum, logical_maximum) > ANALOG_BUTTON_PRESS_THRESHOLD;
}

// https://w3c.github.io/gamepad/#dfn-gamepad-user-gesture
bool contains_gamepad_user_gesture(GamepadDescription const& description, GamepadState const& state)
{
    // A gamepad contains a gamepad user gesture if the current input state indicates that the user is currently
    // interacting with the gamepad. The user agent MUST provide an algorithm to check if the input state contains a
    // gamepad user gesture. For buttons that support a neutral default value and have reported a pressed value of
    // false at least once, a pressed value of true SHOULD be considered interaction. If a button does not support a
    // neutral default value (for example, a toggle switch), then a pressed value of true SHOULD NOT be considered
    // interaction. If a button has never reported a pressed value of false then it SHOULD NOT be considered
    // interaction. Axis movements SHOULD be considered interaction if the axis supports a neutral default value, the
    // current displacement from neutral is greater than a threshold chosen by the user agent, and the axis has
    // reported a value below the threshold at least once. If an axis does not support a neutral default value (for
    // example, an axis for a joystick that does not self-center), or an axis has never reported a value below the axis
    // gesture threshold, then the axis SHOULD NOT be considered when checking for interaction. The axis gesture
    // threshold SHOULD be large enough that random jitter is not considered interaction.

    // NB: This roughly follows Chrome, where it exposes gamepads if a button is pressed (even if it's held across a
    //     refresh) or an absolute axis is above 0.5.
    for (size_t button_index = 0; button_index < min(state.button_values.size(), description.buttons.size()); ++button_index) {
        auto const& button = description.buttons[button_index];
        if (is_button_pressed(state.button_values[button_index], button.logical_minimum, button.logical_maximum))
            return true;
    }

    for (size_t axis_index = 0; axis_index < min(state.axis_values.size(), description.axes.size()); ++axis_index) {
        auto const& axis = description.axes[axis_index];
        if (abs(normalize_axis_value(state.axis_values[axis_index], axis.logical_minimum, axis.logical_maximum)) > AXIS_GESTURE_THRESHOLD)
            return true;
    }

    return false;
}

}
