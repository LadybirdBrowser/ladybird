/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebView/Forward.h>

struct SDL_Gamepad;
struct SDL_Joystick;

union SDL_Event;

namespace WebView {

class WebContentClient;

class WEBVIEW_API GamepadManager {
public:
    static GamepadManager& the();

    void client_did_start_using_gamepads(WebContentClient&);
    void client_disconnected(WebContentClient&);

    void play_effect(WebContentClient&, Web::Gamepad::GamepadHandle, Web::Gamepad::GamepadEffect const&);
    void stop_effects(WebContentClient&, Web::Gamepad::GamepadHandle);

    Optional<Web::Gamepad::VirtualGamepad> create_virtual_gamepad(WebContentClient&);
    void set_virtual_gamepad_button(WebContentClient&, Web::Gamepad::GamepadHandle, i32 button, bool down);
    void set_virtual_gamepad_axis(WebContentClient&, Web::Gamepad::GamepadHandle, i32 axis, i16 value);
    void disconnect_virtual_gamepad(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::GamepadChangeEvent> pump_gamepad_events(WebContentClient&);
    Vector<Web::Gamepad::ReceivedDualRumbleEffect> virtual_gamepad_received_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> virtual_gamepad_received_trigger_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);

    struct Device {
        AK_ALLOC_WITH_KMALLOC;

        u32 sdl_joystick_id { 0 };
        SDL_Gamepad* sdl_gamepad { nullptr };
        Web::Gamepad::GamepadDescription description;
        bool announced { false };
        WebContentClient* dual_rumble_owner { nullptr };
        WebContentClient* trigger_rumble_owner { nullptr };
        Web::Gamepad::GamepadState last_published_state;

        WebContentClient* virtual_device_owner { nullptr };
        SDL_Joystick* virtual_sdl_joystick { nullptr };
        Vector<Web::Gamepad::ReceivedDualRumbleEffect> received_dual_rumble_effects;
        Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> received_trigger_rumble_effects;
    };

    struct Consumer {
        AK_ALLOC_WITH_KMALLOC;

        Core::AnonymousBuffer state_buffer;
        Array<Web::Gamepad::GamepadHandle, Web::Gamepad::MAX_SHARED_GAMEPADS> slot_handles {};
        bool receives_real_devices { false };
        Vector<Web::Gamepad::GamepadChangeEvent> buffered_virtual_gamepad_events;
    };

private:
    bool ensure_sdl_initialized();
    void update_polling_state();
    void drain_sdl_events();
    void handle_sdl_event(SDL_Event const&);
    void publish_changed_input_states();
    void publish_device_state(Device const&);

    Consumer* ensure_consumer(WebContentClient&);

    static Optional<size_t> allocate_shared_state_slot(Consumer&, Web::Gamepad::GamepadHandle);
    static Optional<size_t> slot_index_for_device(Consumer const&, Web::Gamepad::GamepadHandle);
    static Web::Gamepad::SharedGamepadStateSlot& shared_state_slot(Consumer&, size_t slot_index);

    void device_connected(u32 sdl_joystick_id);
    void device_disconnected(u32 sdl_joystick_id);

    Web::Gamepad::GamepadDescription build_description(Web::Gamepad::GamepadHandle, u32 sdl_joystick_id, SDL_Gamepad*) const;
    void read_current_state(Device const&, Web::Gamepad::GamepadState&) const;

    Device* device_for_handle(Web::Gamepad::GamepadHandle);
    Device* virtual_device_owned_by_client(WebContentClient&, Web::Gamepad::GamepadHandle);
    Device* device_announced_to_client(WebContentClient&, Web::Gamepad::GamepadHandle);
    Device* device_for_sdl_joystick_id(u32 sdl_joystick_id);

    void announce_device(Device&);
    void announce_device_to_consumer(Device const&, WebContentClient&, Consumer&);
    void fill_vacant_slots(WebContentClient&, Consumer&);
    static bool consumer_may_receive_device(Consumer const&, WebContentClient const&, Device const&);
    void remove_device(Web::Gamepad::GamepadHandle);

    bool m_sdl_initialized { false };
    bool m_in_test_mode { false };
    Web::Gamepad::GamepadHandle m_next_handle { 1 };

    OrderedHashMap<Web::Gamepad::GamepadHandle, NonnullOwnPtr<Device>> m_devices;
    HashMap<WebContentClient*, NonnullOwnPtr<Consumer>> m_consumers;
    RefPtr<Core::Timer> m_poll_timer;
    Web::Gamepad::GamepadState m_scratch_state;
};

}
