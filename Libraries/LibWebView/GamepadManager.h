/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Forward.h>
#include <LibWebView/GamepadSource.h>

namespace WebView {

class WebContentClient;

class WEBVIEW_API GamepadManager {
public:
    static GamepadManager& the();

    // Without a source, pages find no gamepads.
    void set_source(OwnPtr<GamepadSource>);

    void page_did_start_using_gamepads(WebContentPage&);
    void page_closed(WebContentPage&);
    void client_disconnected(WebContentClient&);
    void client_did_connect_test_endpoint(WebContentClient&);

    void play_effect(WebContentClient&, Web::Gamepad::GamepadHandle, Web::Gamepad::GamepadEffect const&);
    void stop_effects(WebContentClient&, Web::Gamepad::GamepadHandle);

    Optional<Web::Gamepad::VirtualGamepad> create_virtual_gamepad(WebContentClient&);
    void set_virtual_gamepad_button(WebContentClient&, Web::Gamepad::GamepadHandle, i32 button, bool down);
    void set_virtual_gamepad_axis(WebContentClient&, Web::Gamepad::GamepadHandle, i32 axis, i16 value);
    void disconnect_virtual_gamepad(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::GamepadChangeEvent> pump_gamepad_events(WebContentClient&);
    void reset_access_for_testing(WebContentPage&);
    Vector<Web::Gamepad::ReceivedDualRumbleEffect> virtual_gamepad_received_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> virtual_gamepad_received_trigger_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);

    struct Device {
        AK_ALLOC_WITH_KMALLOC;

        bool contains_gamepad_user_gesture() const;
        bool may_be_announced_to(WebContentClient const&) const;

        Web::Gamepad::GamepadDescription description;
        bool announced { false };
        WebContentClient* dual_rumble_owner { nullptr };
        WebContentClient* trigger_rumble_owner { nullptr };
        Web::Gamepad::GamepadState last_published_state;

        WebContentClient* virtual_device_owner { nullptr };
    };

    struct Consumer {
        AK_ALLOC_WITH_KMALLOC;

        explicit Consumer(WebContentClient& client)
            : client(client)
        {
        }

        Optional<size_t> allocate_shared_state_slot(Web::Gamepad::GamepadHandle);
        Optional<size_t> slot_index_for_device(Web::Gamepad::GamepadHandle) const;
        Web::Gamepad::SharedGamepadStateSlot& shared_state_slot(size_t slot_index);
        bool may_receive_device(Device const&) const;
        bool has_page_that_may_receive_gamepads() const;
        void notify_about_user_activation_for_gamepad_user_gesture() const;

        WebContentClient& client;
        Core::AnonymousBuffer state_buffer;
        Array<Web::Gamepad::GamepadHandle, Web::Gamepad::MAX_SHARED_GAMEPADS> slot_handles {};
        HashTable<Web::PageId> pages_using_gamepads;
        bool has_seen_gamepad_user_gesture { false };
        bool has_pending_state_changes { false };
        Vector<Web::Gamepad::GamepadChangeEvent> buffered_virtual_gamepad_events;
    };

private:
    bool ensure_source_started();
    void update_polling_state();
    void gamepads_changed(Vector<Web::Gamepad::GamepadState>);
    void publish_device_state(Device const&);
    void notify_consumers_of_state_changes();

    Consumer* ensure_consumer(WebContentClient&);

    void device_connected(Web::Gamepad::GamepadDescription, Web::Gamepad::GamepadState);

    Device* device_for_handle(Web::Gamepad::GamepadHandle);
    Device* virtual_device_owned_by_client(WebContentClient&, Web::Gamepad::GamepadHandle);
    Device* device_announced_to_client(WebContentClient&, Web::Gamepad::GamepadHandle);

    void announce_device(Device&);
    void announce_device_to_consumer(Device const&, Consumer&);
    void fill_vacant_slots(Consumer&);
    void announce_devices_after_gamepad_user_gesture(Consumer&, Device const&);
    void remove_device(Web::Gamepad::GamepadHandle);

    OwnPtr<GamepadSource> m_source;
    bool m_source_started { false };

    OrderedHashMap<Web::Gamepad::GamepadHandle, NonnullOwnPtr<Device>> m_devices;
    HashMap<WebContentClient*, NonnullOwnPtr<Consumer>> m_consumers;
};

}
