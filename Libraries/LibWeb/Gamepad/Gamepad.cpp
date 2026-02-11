/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Gamepad/EventNames.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadButton.h>
#include <LibWeb/Gamepad/GamepadEvent.h>
#include <LibWeb/Gamepad/GamepadHapticActuator.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(Gamepad);

// https://w3c.github.io/gamepad/#dfn-standard-gamepad
// Type     Index   Location
// Button   0       Bottom button in right cluster
//          1       Right button in right cluster
//          2       Left button in right cluster
//          3       Top button in right cluster
//          4       Top left front button
//          5       Top right front button
//          6       Bottom left front button
//          7       Bottom right front button
//          8       Left button in center cluster
//          9       Right button in center cluster
//          10      Left stick pressed button
//          11      Right stick pressed button
//          12      Top button in left cluster
//          13      Bottom button in left cluster
//          14      Left button in left cluster
//          15      Right button in left cluster
//          16      Center button in center cluster
static Array<Variant<SDL_GamepadButton, SDL_GamepadAxis, Empty>, 17> standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_GUIDE,
};

static Array<SDL_GamepadButton, 11> non_standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,
    SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6,
};

// axes     0       Horizontal axis for left stick (negative left/positive right)
//          1       Vertical axis for left stick (negative up/positive down)
//          2       Horizontal axis for right stick (negative left/positive right)
//          3       Vertical axis for right stick (negative up/positive down)
static Array<SDL_GamepadAxis, 4> standard_gamepad_axes_layout {
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
};

// https://w3c.github.io/gamepad/#dfn-button-press-threshold
// For buttons which do not have a digital switch to indicate a pure pressed or released state, the user
// agent MUST choose a button press threshold to indicate the button as pressed when its value is above a
// certain amount. If the platform API gives a recommended value, the user agent SHOULD use that. In other
// cases, the user agent SHOULD choose some other reasonable value.
static constexpr double ANALOG_BUTTON_PRESS_THRESHOLD = 0.1;

static constexpr double GAMEPAD_EXPOSURE_AXIS_THRESHOLD = 0.5;

// https://w3c.github.io/gamepad/#dfn-a-new-gamepad
GC::Ref<Gamepad> Gamepad::create(JS::Realm& realm, SDL_JoystickID sdl_joystick_id)
{
    // 1. Let gamepad be a newly created Gamepad instance:
    auto gamepad = realm.create<Gamepad>(realm, sdl_joystick_id);

    //    1. Initialize gamepad's id attribute to an identification string for the gamepad.
    //    FIXME: What is the encoding used by SDL?
    auto const* name = SDL_GetGamepadNameForID(sdl_joystick_id);
    if (name) {
        gamepad->m_id = Utf16String::from_utf8(StringView { name, strlen(name) });
    }

    //    2. Initialize gamepad's index attribute to the result of selecting an unused gamepad index for gamepad.
    //    https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
    //    1. Let navigator be gamepad's relevant global object's Navigator object.
    //    The rest of the steps are implemented in NavigatorGamepad.
    //    NOTE: Gamepad is only exposed on Window.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(gamepad));
    gamepad->m_index = window.navigator()->select_an_unused_gamepad_index({});

    //    3. Initialize gamepad's mapping attribute to the result of selecting a mapping for the gamepad device.
    gamepad->select_a_mapping();

    //    4. Set gamepad.[[connected]] to true.
    gamepad->m_connected = true;

    //    5. Set gamepad.[[timestamp]] to the current high resolution time given gamepad's relevant global object.
    gamepad->m_timestamp = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(gamepad));

    //    6. Set gamepad.[[axes]] to the result of initializing axes for gamepad.
    gamepad->initialize_axes();

    //    7. Set gamepad.[[buttons]] to the result of initializing buttons for gamepad.
    gamepad->initialize_buttons();

    //    8. Set gamepad.[[vibrationActuator]] to the result of constructing a GamepadHapticActuator for gamepad.
    gamepad->m_vibration_actuator = GamepadHapticActuator::create(realm, gamepad);

    // 2. Return gamepad.
    return gamepad;
}

Gamepad::Gamepad(JS::Realm& realm, SDL_JoystickID sdl_joystick_id)
    : PlatformObject(realm)
    , m_sdl_joystick_id(sdl_joystick_id)
{
    m_sdl_gamepad = SDL_OpenGamepad(m_sdl_joystick_id);
}

void Gamepad::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Gamepad);
    Base::initialize(realm);
}

void Gamepad::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buttons);
    visitor.visit(m_vibration_actuator);
}

void Gamepad::finalize()
{
    Base::finalize();
    SDL_CloseGamepad(m_sdl_gamepad);
}

// https://w3c.github.io/gamepad/#dfn-initializing-axes
void Gamepad::initialize_axes()
{
    // 1. Let inputCount be the number of axis inputs exposed by the device represented by gamepad.
    Vector<SDL_GamepadAxis> inputs;

    // 2. Set gamepad.[[axisMinimums]] to a list of unsigned long values with size equal to inputCount containing minimum logical values for each of the axis inputs.
    // 3. Set gamepad.[[axisMaximums]] to a list of unsigned long values with size equal to inputCount containing maximum logical values for each of the axis inputs.
    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout) {
        if (SDL_GamepadHasAxis(m_sdl_gamepad, standard_gamepad_axis)) {
            inputs.append(standard_gamepad_axis);
            m_axis_minimums.append(SDL_JOYSTICK_AXIS_MIN);
            m_axis_maximums.append(SDL_JOYSTICK_AXIS_MAX);
        }
    }

    // 4. Let unmappedInputList be an empty list.
    Vector<size_t> unmapped_input_list;

    // 5. Let mappedIndexList be an empty list.
    Vector<size_t> mapped_index_list;

    // 6. Let axesSize be 0.
    size_t axes_size = 0;

    // 7. For each rawInputIndex of the range from 0 to inputCount − 1:
    for (size_t raw_input_index = 0; raw_input_index < inputs.size(); ++raw_input_index) {
        // 1. If the gamepad axis at index rawInputIndex represents a Standard Gamepad axis:
        auto const axis = inputs[raw_input_index];
        if (auto maybe_index = standard_gamepad_axes_layout.first_index_of(axis); maybe_index.has_value()) {
            // 1. Let canonicalIndex be the canonical index for the axis.
            auto canonical_index = maybe_index.value();

            // 2. If mappedIndexList contains canonicalIndex, then append rawInputIndex to unmappedInputList.
            if (mapped_index_list.contains_slow(canonical_index)) {
                unmapped_input_list.append(raw_input_index);
            } else {
                // Otherwise:
                // 1. Set gamepad.[[axisMapping]][rawInputIndex] to canonicalIndex.
                m_axis_mapping.set(raw_input_index, canonical_index);

                // 2. Append canonicalIndex to mappedIndexList.
                mapped_index_list.append(canonical_index);

                // 3. If canonicalIndex + 1 is greater than axesSize, then set axesSize to canonicalIndex + 1.
                if (canonical_index + 1 > axes_size)
                    axes_size = canonical_index + 1;
            }
        } else {
            // Otherwise, append rawInputIndex to unmappedInputList.
            unmapped_input_list.append(raw_input_index);
        }
    }

    // 8. Let axisIndex be 0.
    size_t axis_index = 0;

    // 9. For each rawInputIndex of unmappedInputList:
    for (size_t raw_input_index : unmapped_input_list) {
        // 1. While mappedIndexList contains axisIndex:
        while (mapped_index_list.contains_slow(axis_index)) {
            // 1. Increment axisIndex.
            ++axis_index;
        }

        // 2. Set gamepad.[[axisMapping]][rawInputIndex] to axisIndex.
        m_axis_mapping.set(raw_input_index, axis_index);

        // 3. Append axisIndex to mappedIndexList.
        mapped_index_list.append(axis_index);

        // 4. If axisIndex + 1 is greater than axesSize, then set axesSize to axisIndex + 1.
        if (axis_index + 1 > axes_size)
            axes_size = axis_index + 1;
    }

    // NOTE: Instead of returning a list, we can just directly update m_buttons.
    // 10. Let axes be an empty list.
    // 11. For each axisIndex of the range from 0 to axesSize − 1, append 0 to axes.
    // 12. Return axes.
    for (size_t final_axis_index = 0; final_axis_index < axes_size; ++final_axis_index)
        m_axes.append(0.0);
}

// https://w3c.github.io/gamepad/#dfn-initializing-buttons
void Gamepad::initialize_buttons()
{
    auto& realm = this->realm();

    // 1. Let inputCount be the number of button inputs exposed by the device represented by gamepad.
    Vector<Variant<SDL_GamepadButton, SDL_GamepadAxis>> inputs;

    // 2. Set gamepad.[[buttonMinimums]] to be a list of unsigned long values with size equal to inputCount containing minimum logical values for each of the button inputs.
    // 3. Set gamepad.[[buttonMaximums]] to be a list of unsigned long values with size equal to inputCount containing maximum logical values for each of the button inputs.
    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [this, &inputs](SDL_GamepadButton button) {
                if (SDL_GamepadHasButton(m_sdl_gamepad, button)) {
                    inputs.append(button);

                    // Buttons are binary inputs with SDL.
                    m_button_minimums.append(0);
                    m_button_maximums.append(1);
                }
            },
            [this, &inputs](SDL_GamepadAxis axis) {
                if (SDL_GamepadHasAxis(m_sdl_gamepad, axis)) {
                    inputs.append(axis);

                    // "Trigger axis values range from 0 (released) to SDL_JOYSTICK_AXIS_MAX (fully
                    // pressed) when reported by SDL_GetGamepadAxis(). Note that this is not the
                    // same range that will be reported by the lower-level SDL_GetJoystickAxis()."
                    m_button_minimums.append(0);
                    m_button_maximums.append(SDL_JOYSTICK_AXIS_MAX);
                }
            },
            [](Empty) {
                VERIFY_NOT_REACHED();
            });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (SDL_GamepadHasButton(m_sdl_gamepad, non_standard_gamepad_button)) {
            inputs.append(non_standard_gamepad_button);

            // Buttons are binary inputs with SDL.
            m_button_minimums.append(0);
            m_button_maximums.append(1);
        }
    }

    // 4. Let unmappedInputList be an empty list.
    Vector<size_t> unmapped_input_list;

    // 5. Let mappedIndexList be an empty list.
    Vector<size_t> mapped_index_list;

    // 6. Let buttonsSize be 0.
    size_t buttons_size = 0;

    // 7. For each rawInputIndex of the range from 0 to inputCount − 1:
    for (size_t raw_input_index = 0; raw_input_index < inputs.size(); ++raw_input_index) {
        auto const& input = inputs[raw_input_index];

        // 1. If the gamepad button at index rawInputIndex represents a Standard Gamepad button:
        if (auto maybe_index = standard_gamepad_button_layout.first_index_of(input); maybe_index.has_value()) {
            // 1. Let canonicalIndex be the canonical index for the button.
            auto canonical_index = maybe_index.value();

            // 2. If mappedIndexList contains canonicalIndex, then append rawInputIndex to unmappedInputList.
            if (mapped_index_list.contains_slow(canonical_index)) {
                unmapped_input_list.append(raw_input_index);
            } else {
                // Otherwise:
                // 1. Set gamepad.[[buttonMapping]][rawInputIndex] to canonicalIndex.
                m_button_mapping.set(raw_input_index, canonical_index);

                // 2. Append canonicalIndex to mappedIndexList.
                mapped_index_list.append(canonical_index);

                // 3. If canonicalIndex + 1 is greater than buttonsSize, then set buttonsSize to canonicalIndex + 1.
                if (canonical_index + 1 > buttons_size)
                    buttons_size = canonical_index + 1;
            }
        } else {
            // Otherwise, append rawInputIndex to unmappedInputList.
            unmapped_input_list.append(raw_input_index);
        }

        // 2. Increment rawInputIndex.
    }

    // 8. Let buttonIndex be 0.
    size_t button_index = 0;

    // 9. For each rawInputIndex of unmappedInputList:
    for (size_t raw_input_index : unmapped_input_list) {
        // 1. While mappedIndexList contains buttonIndex:
        while (mapped_index_list.contains_slow(button_index)) {
            // 1. Increment buttonIndex.
            ++button_index;
        }

        // 2. Set gamepad.[[buttonMapping]][rawInputIndex] to buttonIndex.
        m_button_mapping.set(raw_input_index, button_index);

        // 3. Append buttonIndex to mappedIndexList.
        mapped_index_list.append(button_index);

        // 4. If buttonIndex + 1 is greater than buttonsSize, then set buttonsSize to buttonIndex + 1.
        if (button_index + 1 > buttons_size)
            buttons_size = button_index + 1;
    }

    // NOTE: Instead of returning a list (and thus needing to use RootVector), we can just directly update m_buttons.
    // 10. Let buttons be an empty list.
    // 11. For each buttonIndex of the range from 0 to buttonsSize − 1, append a new GamepadButton to buttons.
    // 12. Return buttons.
    for (size_t final_button_index = 0; final_button_index < buttons_size; ++final_button_index) {
        auto gamepad_button = realm.create<GamepadButton>(realm);
        m_buttons.append(gamepad_button);
    }
}

GC::Ref<GamepadHapticActuator> Gamepad::vibration_actuator() const
{
    VERIFY(m_vibration_actuator);
    return *m_vibration_actuator;
}

void Gamepad::set_connected(Badge<NavigatorGamepadPartial>, bool value)
{
    m_connected = value;
}

void Gamepad::set_exposed(Badge<NavigatorGamepadPartial>, bool value)
{
    m_exposed = value;
}

void Gamepad::set_timestamp(Badge<NavigatorGamepadPartial>, HighResolutionTime::DOMHighResTimeStamp value)
{
    m_timestamp = value;
}

// https://w3c.github.io/gamepad/#dfn-selecting-a-mapping
void Gamepad::select_a_mapping()
{
    // 1. If the button and axis layout of the gamepad device corresponds with the Standard Gamepad layout, then
    //    return "standard".
    // 2. Return "".
    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        bool has_standard_button = standard_gamepad_button.visit(
            [this](SDL_GamepadButton button) -> bool {
                return SDL_GamepadHasButton(m_sdl_gamepad, button);
            },
            [this](SDL_GamepadAxis axis) -> bool {
                return SDL_GamepadHasAxis(m_sdl_gamepad, axis);
            },
            [](Empty) -> bool {
                VERIFY_NOT_REACHED();
            });

        if (!has_standard_button) {
            m_mapping = Bindings::GamepadMappingType::Empty;
            return;
        }
    }

    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout) {
        if (!SDL_GamepadHasAxis(m_sdl_gamepad, standard_gamepad_axis)) {
            m_mapping = Bindings::GamepadMappingType::Empty;
            return;
        }
    }

    m_mapping = Bindings::GamepadMappingType::Standard;
}

// https://w3c.github.io/gamepad/#dfn-map-and-normalize-axes
void Gamepad::map_and_normalize_axes()
{
    // 1. Let axisValues be a list of unsigned long values representing the most recent logical axis input values for
    //    each axis input of the device represented by gamepad.
    // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
    Vector<i16> axis_values;
    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout) {
        if (SDL_GamepadHasAxis(m_sdl_gamepad, standard_gamepad_axis))
            axis_values.append(SDL_GetGamepadAxis(m_sdl_gamepad, standard_gamepad_axis));
    }

    // 2. Let maxRawAxisIndex be the size of axisValues − 1.
    // 3. For each rawAxisIndex of the range from 0 to maxRawAxisIndex:
    for (size_t raw_axis_index = 0; raw_axis_index < axis_values.size(); ++raw_axis_index) {
        // 1. Let mappedIndex be gamepad.[[axisMapping]][rawAxisIndex].
        auto mapped_index = m_axis_mapping.get(raw_axis_index).value();

        // 2. Let logicalValue be axisValues[rawAxisIndex].
        auto logical_value = axis_values[raw_axis_index];

        // 3. Let logicalMinimum be gamepad.[[axisMinimums]][rawAxisIndex].
        auto logical_minimum = m_axis_minimums[raw_axis_index];

        // 4. Let logicalMaximum be gamepad.[[axisMaximums]][rawAxisIndex].
        auto logical_maximum = m_axis_maximums[raw_axis_index];

        // 5. Let normalizedValue be 2 (logicalValue − logicalMinimum) / (logicalMaximum − logicalMinimum) − 1.
        double normalized_value = 2.0 * static_cast<double>(logical_value - logical_minimum) / static_cast<double>(logical_maximum - logical_minimum) - 1.0;

        // 6. Set gamepad.[[axes]][axisIndex] to be normalizedValue.
        // FIXME: axisIndex should be mappedIndex.
        m_axes[mapped_index] = normalized_value;
    }
}

// https://w3c.github.io/gamepad/#dfn-map-and-normalize-buttons
void Gamepad::map_and_normalize_buttons()
{
    // 1. Let buttonValues be a list of unsigned long values representing the most recent logical button input values
    //    for each button input of the device represented by gamepad.
    // NOTE: While the Gamepad API internally uses u32 to represent raw button values, SDL uses bool for buttons and
    //       i16 for axes. The left and right triggers are buttons in the Gamepad API.
    Vector<i16> button_values;

    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [this, &button_values](SDL_GamepadButton button) {
                if (SDL_GamepadHasButton(m_sdl_gamepad, button)) {
                    bool button_pressed = SDL_GetGamepadButton(m_sdl_gamepad, button);
                    button_values.append(button_pressed ? 1 : 0);
                }
            },
            [this, &button_values](SDL_GamepadAxis axis) {
                if (SDL_GamepadHasAxis(m_sdl_gamepad, axis))
                    button_values.append(SDL_GetGamepadAxis(m_sdl_gamepad, axis));
            },
            [](Empty) {
                VERIFY_NOT_REACHED();
            });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (SDL_GamepadHasButton(m_sdl_gamepad, non_standard_gamepad_button)) {
            bool button_pressed = SDL_GetGamepadButton(m_sdl_gamepad, non_standard_gamepad_button);
            button_values.append(button_pressed ? 1 : 0);
        }
    }

    // 2. Let maxRawButtonIndex be the size of buttonValues − 1.
    // 3. For each rawButtonIndex of the range from 0 to maxRawButtonIndex:
    for (size_t raw_button_index = 0; raw_button_index < button_values.size(); ++raw_button_index) {
        // 1. Let mappedIndex be gamepad.[[buttonMapping]][rawButtonIndex].
        auto mapped_index = m_button_mapping.get(raw_button_index).value();

        // 2. Let logicalValue be buttonValues[rawButtonIndex].
        auto logical_value = button_values[raw_button_index];

        // 3. Let logicalMinimum be gamepad.[[buttonMinimums]][rawButtonIndex].
        auto logical_minimum = m_button_minimums[raw_button_index];

        // 4. Let logicalMaximum be gamepad.[[buttonMaximums]][rawButtonIndex].
        auto logical_maximum = m_button_maximums[raw_button_index];

        // 5. Let normalizedValue be (logicalValue − logicalMinimum) / (logicalMaximum − logicalMinimum).
        double value = static_cast<double>(logical_value - logical_minimum) / static_cast<double>(logical_maximum - logical_minimum);

        // 6. Let button be gamepad.[[buttons]][mappedIndex].
        auto button = m_buttons[mapped_index];

        // 7. Set button.[[value]] to normalizedValue.
        button->set_value({}, value);

        // 8. If the button has a digital switch to indicate a pure pressed or released state, set button.[[pressed]]
        //    to true if the button is pressed or false if it is not pressed.
        //    Otherwise, set button.[[pressed]] to true if the value is above the button press threshold or false if
        //    it is not above the threshold.
        if (logical_maximum == 1) {
            button->set_pressed({}, logical_value == 1);
        } else {
            button->set_pressed({}, value > ANALOG_BUTTON_PRESS_THRESHOLD);
        }

        // 9. If the button is capable of detecting touch, set button.[[touched]] to true if the button is currently being touched.
        //    Otherwise, set button.[[touched]] to button.[[pressed]].
        // FIXME: Support the PS4/PS5 controller which has a touchpad, which is a button that can be touched and not pressed in at the same time.
        button->set_touched({}, button->pressed());
    }
}

// https://w3c.github.io/gamepad/#dfn-update-gamepad-state
void Gamepad::update_gamepad_state(Badge<NavigatorGamepadPartial>)
{
    auto& realm = this->realm();

    // 1. Let now be the current high resolution time given gamepad's relevant global object.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    auto now = HighResolutionTime::current_high_resolution_time(window);

    // 2. Set gamepad.[[timestamp]] to now.
    m_timestamp = now;

    // 3. Run the steps to map and normalize axes for gamepad.
    map_and_normalize_axes();

    // 4. Run the steps to map and normalize buttons for gamepad.
    map_and_normalize_buttons();

    // FIXME: 5. Run the steps to record touches for gamepad.

    // 6. Let navigator be gamepad's relevant global object's Navigator object.
    auto navigator = window.navigator();

    // 7. If navigator.[[hasGamepadGesture]] is false and gamepad contains a gamepad user gesture:
    if (!navigator->has_gamepad_gesture() && contains_gamepad_user_gesture()) {
        // 1. Set navigator.[[hasGamepadGesture]] to true.
        navigator->set_has_gamepad_gesture({}, true);

        // 2. For each connectedGamepad of navigator.[[gamepads]]:
        for (auto connected_gamepad : navigator->gamepads({})) {
            // 1. If connectedGamepad is not equal to null:
            if (connected_gamepad) {
                // 1. Set connectedGamepad.[[exposed]] to true.
                connected_gamepad->m_exposed = true;

                // 2. Set connectedGamepad.[[timestamp]] to now.
                connected_gamepad->m_timestamp = now;

                // 3. Let document be gamepad's relevant global object's associated Document; otherwise null.
                auto& document = window.associated_document();

                // 4. If document is not null and is fully active, then queue a global task on the gamepad task source
                //    to fire an event named gamepadconnected at gamepad's relevant global object using GamepadEvent
                //    with its gamepad attribute initialized to connectedGamepad.
                if (document.is_fully_active()) {
                    auto gamepad_connected_event_init = GamepadEventInit {
                        {
                            .bubbles = false,
                            .cancelable = false,
                            .composed = false,
                        },
                        connected_gamepad,
                    };
                    auto gamepad_connected_event = MUST(GamepadEvent::construct_impl(realm, EventNames::gamepadconnected, gamepad_connected_event_init));
                    window.dispatch_event(gamepad_connected_event);
                }
            }
        }
    }
}

// https://w3c.github.io/gamepad/#dfn-gamepad-user-gesture
bool Gamepad::contains_gamepad_user_gesture()
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

    // NOTE: This roughly follows Chrome, where it exposes gamepads if a button is pressed (even if it's held across
    //       a refresh) or an absolute axis is above 0.5.
    auto pressed_button = m_buttons.find_if([](GC::Ref<GamepadButton> gamepad_button) {
        return gamepad_button->pressed();
    });

    if (!pressed_button.is_end())
        return true;

    auto axis_above_threshold = m_axes.find_if([](double value) {
        return abs(value) > GAMEPAD_EXPOSURE_AXIS_THRESHOLD;
    });

    return !axis_above_threshold.is_end();
}

}
