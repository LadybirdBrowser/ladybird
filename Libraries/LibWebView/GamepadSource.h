/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebView/Export.h>

namespace WebView {

// The gamepads of the system, and the virtual ones that tests make. The GamepadManager decides which pages learn of
// them.
class WEBVIEW_API GamepadSource {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~GamepadSource() = default;

    // Starts looking for gamepads. False if the platform can't.
    virtual bool start() = 0;

    // Whether to look at the gamepads by itself, as often as a game needs. Otherwise it looks only in poll().
    virtual void set_polling(bool) = 0;

    // Reports what changed since it last looked, if it can look right away.
    virtual void poll() = 0;

    // A duration of 0 with magnitudes of 0 stops the effect.
    virtual void rumble(Web::Gamepad::GamepadHandle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms) = 0;
    virtual void rumble_triggers(Web::Gamepad::GamepadHandle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms) = 0;

    // Virtual gamepads, for tests. They report a connection like any other gamepad.
    virtual Optional<Web::Gamepad::VirtualGamepad> create_virtual_gamepad() { return {}; }
    virtual void set_virtual_gamepad_button(Web::Gamepad::GamepadHandle, i32, bool) { }
    virtual void set_virtual_gamepad_axis(Web::Gamepad::GamepadHandle, i32, i16) { }
    virtual void disconnect_virtual_gamepad(Web::Gamepad::GamepadHandle) { }
    virtual Vector<Web::Gamepad::ReceivedDualRumbleEffect> virtual_gamepad_received_rumble_effects(Web::Gamepad::GamepadHandle) { return {}; }
    virtual Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> virtual_gamepad_received_trigger_rumble_effects(Web::Gamepad::GamepadHandle) { return {}; }

    Function<void(Web::Gamepad::GamepadDescription, Web::Gamepad::GamepadState)> on_gamepad_connected;
    Function<void(Web::Gamepad::GamepadHandle)> on_gamepad_disconnected;
    // The states that changed since the source last looked.
    Function<void(Vector<Web::Gamepad::GamepadState>)> on_gamepad_states_changed;
};

}
