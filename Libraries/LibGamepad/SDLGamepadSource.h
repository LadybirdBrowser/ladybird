/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibWebView/GamepadSource.h>

struct SDL_Gamepad;
struct SDL_Joystick;
union SDL_Event;

namespace Gamepad {

enum class IgnoreSystemGamepads : u8 {
    No,
    Yes,
};

// Reads the gamepads through SDL, on the main thread.
class SDLGamepadSource final : public WebView::GamepadSource {
public:
    // Tests see only the virtual gamepads they make, and look at them when they ask.
    explicit SDLGamepadSource(IgnoreSystemGamepads);
    virtual ~SDLGamepadSource() override;

    struct Device {
        AK_ALLOC_WITH_KMALLOC;

        u32 sdl_joystick_id { 0 };
        SDL_Gamepad* sdl_gamepad { nullptr };
        Web::Gamepad::GamepadDescription description;
        bool connected { false };
        Web::Gamepad::GamepadState last_state;

        SDL_Joystick* virtual_sdl_joystick { nullptr };
        Vector<Web::Gamepad::ReceivedDualRumbleEffect> received_dual_rumble_effects;
        Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> received_trigger_rumble_effects;
    };

private:
    virtual bool start() override;
    virtual void set_polling(bool) override;
    virtual void poll() override;
    virtual void rumble(Web::Gamepad::GamepadHandle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms) override;
    virtual void rumble_triggers(Web::Gamepad::GamepadHandle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms) override;

    virtual Optional<Web::Gamepad::VirtualGamepad> create_virtual_gamepad() override;
    virtual void set_virtual_gamepad_button(Web::Gamepad::GamepadHandle, i32 button, bool down) override;
    virtual void set_virtual_gamepad_axis(Web::Gamepad::GamepadHandle, i32 axis, i16 value) override;
    virtual void disconnect_virtual_gamepad(Web::Gamepad::GamepadHandle) override;
    virtual Vector<Web::Gamepad::ReceivedDualRumbleEffect> virtual_gamepad_received_rumble_effects(Web::Gamepad::GamepadHandle) override;
    virtual Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> virtual_gamepad_received_trigger_rumble_effects(Web::Gamepad::GamepadHandle) override;

    void handle_sdl_event(SDL_Event const&);
    void device_connected(u32 sdl_joystick_id);
    void device_disconnected(u32 sdl_joystick_id);
    void remove_device(Web::Gamepad::GamepadHandle);
    Device* device_for_sdl_joystick_id(u32 sdl_joystick_id);
    Device* virtual_device(Web::Gamepad::GamepadHandle);
    Web::Gamepad::GamepadDescription build_description(Web::Gamepad::GamepadHandle, u32 sdl_joystick_id, SDL_Gamepad*) const;
    void read_current_state(Device const&, Web::Gamepad::GamepadState&) const;

    IgnoreSystemGamepads m_ignore_system_gamepads { IgnoreSystemGamepads::No };
    bool m_sdl_initialized { false };
    Web::Gamepad::GamepadHandle m_next_handle { 1 };
    OrderedHashMap<Web::Gamepad::GamepadHandle, NonnullOwnPtr<Device>> m_devices;
    RefPtr<Core::Timer> m_poll_timer;
    Web::Gamepad::GamepadState m_scratch_state;
};

}
