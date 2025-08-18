/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GamepadPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-gamepad
class Gamepad final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Gamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Gamepad);

public:
    static GC::Ref<Gamepad> create(JS::Realm&, SDL_JoystickID);

    SDL_JoystickID sdl_joystick_id() const { return m_sdl_joystick_id; }
    SDL_Gamepad* sdl_gamepad() const { return m_sdl_gamepad; }

    Utf16String const& id() const { return m_id; }

    size_t index() const { return m_index; }

    bool connected() const { return m_connected; }
    void set_connected(Badge<NavigatorGamepadPartial>, bool);

    HighResolutionTime::DOMHighResTimeStamp timestamp() const { return m_timestamp; }
    void set_timestamp(Badge<NavigatorGamepadPartial>, HighResolutionTime::DOMHighResTimeStamp);

    bool exposed() const { return m_exposed; }
    void set_exposed(Badge<NavigatorGamepadPartial>, bool);

    Bindings::GamepadMappingType mapping() const { return m_mapping; }

    Vector<double> const& axes() const { return m_axes; }
    Vector<GC::Ref<GamepadButton>> const& buttons() const { return m_buttons; }

    GC::Ref<GamepadHapticActuator> vibration_actuator() const;

    void update_gamepad_state(Badge<NavigatorGamepadPartial>);

private:
    explicit Gamepad(JS::Realm&, SDL_JoystickID);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    void select_a_mapping();
    void initialize_axes();
    void initialize_buttons();

    void map_and_normalize_axes();
    void map_and_normalize_buttons();

    bool contains_gamepad_user_gesture();

    // https://w3c.github.io/gamepad/#dom-gamepad-id
    // An identification string for the gamepad. This string identifies the brand or style of connected gamepad device.
    // The exact format of the id string is left unspecified. It is RECOMMENDED that the user agent select a string
    // that identifies the product without uniquely identifying the device. For example, a USB gamepad may be
    // identified by its idVendor and idProduct values. Unique identifiers like serial numbers or Bluetooth device
    // addresses MUST NOT be included in the id string.
    Utf16String m_id;

    // https://w3c.github.io/gamepad/#dom-gamepad-index
    // The index of the gamepad in the Navigator. When multiple gamepads are connected to a user agent, indices MUST be
    // assigned on a first-come, first-serve basis, starting at zero. If a gamepad is disconnected, previously assigned
    // indices MUST NOT be reassigned to gamepads that continue to be connected. However, if a gamepad is disconnected,
    // and subsequently the same or a different gamepad is then connected, the lowest previously used index MUST be
    // reused.
    size_t m_index { 0 };

    // https://w3c.github.io/gamepad/#dfn-connected
    // A flag indicating that the device is connected to the system
    bool m_connected { false };

    // https://w3c.github.io/gamepad/#dfn-timestamp
    // The last time data for this Gamepad was updated
    HighResolutionTime::DOMHighResTimeStamp m_timestamp { 0.0 };

    // https://w3c.github.io/gamepad/#dfn-axes
    // A sequence of double values representing the current state of axes exposed by this device.
    // https://w3c.github.io/gamepad/#dom-gamepad-axes
    // Array of values for all axes of the gamepad. All axis values MUST be linearly normalized to the range [-1 .. 1].
    // If the controller is perpendicular to the ground with the directional stick pointing up, -1 SHOULD correspond to
    // "forward" or "left", and 1 SHOULD correspond to "backward" or "right". Axes that are drawn from a 2D input
    // device SHOULD appear next to each other in the axes array, X then Y. It is RECOMMENDED that axes appear in
    // decreasing order of importance, such that element 0 and 1 typically represent the X and Y axis of a directional
    // stick. The same object MUST be returned until the user agent needs to return different values (or values in a
    // different order).
    // FIXME: Our current FrozenArray implementation only supports returning new objects everytime.
    Vector<double> m_axes;

    // https://w3c.github.io/gamepad/#dfn-axismapping
    // Mapping from unmapped axis index to an index in the axes array
    HashMap<size_t, size_t> m_axis_mapping;

    // https://w3c.github.io/gamepad/#dfn-axisminimums
    // A list containing the minimum logical value for each axis
    // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
    Vector<i16> m_axis_minimums;

    // https://w3c.github.io/gamepad/#dfn-axismaximums
    // A list containing the maximum logical value for each axis
    // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
    Vector<i16> m_axis_maximums;

    // https://w3c.github.io/gamepad/#dfn-buttons
    // A sequence of GamepadButton objects representing the current state of buttons exposed by this device
    // Array of button states for all buttons of the gamepad. It is RECOMMENDED that buttons appear in decreasing
    // importance such that the primary button, secondary button, tertiary button, and so on appear as elements 0, 1,
    // 2, ... in the buttons array. The same object MUST be returned until the user agent needs to return different
    // values (or values in a different order).
    // FIXME: Our current FrozenArray implementation only supports returning new objects everytime.
    Vector<GC::Ref<GamepadButton>> m_buttons;

    // https://w3c.github.io/gamepad/#dfn-buttonmapping
    // Mapping from unmapped button index to an index in the buttons array
    HashMap<size_t, size_t> m_button_mapping;

    // https://w3c.github.io/gamepad/#dfn-buttonminimums
    // A list containing the minimum logical value for each button.
    // NOTE: While the Gamepad API internally uses u32 to represent raw button values, SDL uses bool for buttons and
    //       i16 for axes. The left and right triggers are buttons in the Gamepad API.
    Vector<i16> m_button_minimums;

    // https://w3c.github.io/gamepad/#dfn-buttonmaximums
    // A list containing the maximum logical value for each button
    Vector<i16> m_button_maximums;

    // https://w3c.github.io/gamepad/#dfn-exposed
    // A flag indicating that the Gamepad object has been exposed to script
    bool m_exposed { false };

    // https://w3c.github.io/gamepad/#dfn-vibrationactuator
    GC::Ptr<GamepadHapticActuator> m_vibration_actuator;

    // https://w3c.github.io/gamepad/#dom-gamepad-mapping
    // The mapping in use for this device. If the user agent has knowledge of the layout of the device, then it SHOULD
    // indicate that a mapping is in use by setting mapping to the corresponding GamepadMappingType value.
    Bindings::GamepadMappingType m_mapping { Bindings::GamepadMappingType::Standard };

    SDL_JoystickID m_sdl_joystick_id { 0 };
    SDL_Gamepad* m_sdl_gamepad { nullptr };
};

}
