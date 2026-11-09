/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/NeverDestroyed.h>
#include <LibWebCommon/Gamepad/GamepadInput.h>
#include <LibWebCommon/Gamepad/GamepadStateBuffer.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// A compromised WebContent process must not be able to request an unbounded rumble.
static constexpr u32 GAMEPAD_EFFECT_MAX_DURATION_MS = 5'000;
static constexpr u32 GAMEPAD_EFFECT_EXPIRATION_SLACK_MS = 1'000;

static u32 effect_expiration_backstop_ms(u32 duration)
{
    return min(duration, GAMEPAD_EFFECT_MAX_DURATION_MS) + GAMEPAD_EFFECT_EXPIRATION_SLACK_MS;
}

GamepadManager& GamepadManager::the()
{
    static NeverDestroyed<GamepadManager> s_the;
    return *s_the;
}

void GamepadManager::set_source(OwnPtr<GamepadSource> source)
{
    m_source = move(source);
    m_source_started = false;
    if (!m_source)
        return;
    m_source->on_gamepad_connected = [this](auto description, auto state) {
        device_connected(move(description), move(state));
        notify_consumers_of_state_changes();
    };
    m_source->on_gamepad_disconnected = [this](auto handle) {
        remove_device(handle);
        notify_consumers_of_state_changes();
    };
    m_source->on_gamepad_states_changed = [this](auto states) {
        gamepads_changed(move(states));
    };
}

bool GamepadManager::ensure_source_started()
{
    if (!m_source)
        return false;
    if (!m_source_started)
        m_source_started = m_source->start();
    return m_source_started;
}

void GamepadManager::update_polling_state()
{
    // A process waiting for a gesture needs the polling that finds it.
    if (m_source_started)
        m_source->set_polling(any_of(m_consumers, [](auto const& entry) { return !entry.value->pages_using_gamepads.is_empty(); }));
}

GamepadManager::Consumer* GamepadManager::ensure_consumer(WebContentClient& client)
{
    if (auto existing_consumer = m_consumers.get(&client); existing_consumer.has_value())
        return *existing_consumer;

    auto state_buffer = Core::AnonymousBuffer::create_with_size(sizeof(Web::Gamepad::SharedGamepadStateBuffer));
    if (state_buffer.is_error()) {
        dbgln("Failed to allocate a shared gamepad state buffer: {}", state_buffer.error());
        return nullptr;
    }

    auto new_consumer = make<Consumer>(client);
    new_consumer->state_buffer = state_buffer.release_value();
    new (new_consumer->state_buffer.data<void>()) Web::Gamepad::SharedGamepadStateBuffer {};
    auto consumer_buffer = Web::Gamepad::GamepadStateBuffer::create(new_consumer->state_buffer);
    if (consumer_buffer.is_error()) {
        dbgln("Failed to share gamepad state buffer: {}", consumer_buffer.error());
        return nullptr;
    }
    client.async_set_gamepad_state_buffer(consumer_buffer.release_value());

    auto* consumer = new_consumer.ptr();
    m_consumers.set(&client, move(new_consumer));
    return consumer;
}

void GamepadManager::page_did_start_using_gamepads(WebContentPage& page)
{
    if (!ensure_source_started())
        return;

    auto& client = page.client();
    auto* consumer = ensure_consumer(client);
    if (!consumer)
        return;

    m_source->poll();

    // A gamepad the user is still interacting with shows a gesture for the page too.
    consumer->pages_using_gamepads.set(page.id());
    for (auto& entry : m_devices) {
        if (entry.value->announced)
            announce_devices_after_gamepad_user_gesture(*consumer, *entry.value);
    }
    notify_consumers_of_state_changes();

    update_polling_state();
}

void GamepadManager::page_closed(WebContentPage& page)
{
    auto consumer = m_consumers.get(&page.client());
    if (!consumer.has_value())
        return;
    (*consumer)->pages_using_gamepads.remove(page.id());
    update_polling_state();
}

void GamepadManager::client_disconnected(WebContentClient& client)
{
    for (auto& entry : m_devices)
        stop_effects(client, entry.key);

    m_consumers.remove(&client);

    Vector<Web::Gamepad::GamepadHandle> owned_virtual_devices;
    for (auto& entry : m_devices) {
        if (entry.value->virtual_device_owner == &client)
            owned_virtual_devices.append(entry.key);
    }
    for (auto handle : owned_virtual_devices) {
        remove_device(handle);
        m_source->disconnect_virtual_gamepad(handle);
    }

    update_polling_state();
}

void GamepadManager::client_did_connect_test_endpoint(WebContentClient& client)
{
    (void)ensure_consumer(client);
}

void GamepadManager::play_effect(WebContentClient& client, Web::Gamepad::GamepadHandle handle, Web::Gamepad::GamepadEffect const& effect)
{
    auto* device = device_announced_to_client(client, handle);
    if (!device)
        return;

    // The expiration only silences the device if the client dies or hangs before ending the effect.
    auto apply_rumble = [&](WebContentClient*& effect_owner, auto rumble_function, u16 first_magnitude, u16 second_magnitude, u32 duration) {
        bool is_stop = first_magnitude == 0 && second_magnitude == 0;
        if (is_stop && effect_owner != &client)
            return;
        effect_owner = is_stop ? nullptr : &client;
        (m_source->*rumble_function)(handle, first_magnitude, second_magnitude, is_stop ? 0 : effect_expiration_backstop_ms(duration));
    };
    effect.visit(
        [&](Web::Gamepad::GamepadDualRumbleEffect const& dual_rumble_effect) {
            apply_rumble(device->dual_rumble_owner, &GamepadSource::rumble, dual_rumble_effect.strong_magnitude, dual_rumble_effect.weak_magnitude, dual_rumble_effect.duration);
        },
        [&](Web::Gamepad::GamepadTriggerRumbleEffect const& trigger_rumble_effect) {
            apply_rumble(device->trigger_rumble_owner, &GamepadSource::rumble_triggers, trigger_rumble_effect.left_trigger_magnitude, trigger_rumble_effect.right_trigger_magnitude, trigger_rumble_effect.duration);
        });
}

void GamepadManager::stop_effects(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = device_announced_to_client(client, handle);
    if (!device)
        return;

    if (device->dual_rumble_owner == &client) {
        device->dual_rumble_owner = nullptr;
        m_source->rumble(handle, 0, 0, 0);
    }

    if (device->trigger_rumble_owner == &client) {
        device->trigger_rumble_owner = nullptr;
        m_source->rumble_triggers(handle, 0, 0, 0);
    }
}

Optional<Web::Gamepad::VirtualGamepad> GamepadManager::create_virtual_gamepad(WebContentClient& client)
{
    if (!ensure_source_started())
        return {};

    if (!ensure_consumer(client))
        return {};

    auto virtual_gamepad = m_source->create_virtual_gamepad();
    if (!virtual_gamepad.has_value())
        return {};

    // The source tells of the connection, which announces the gamepad to its owner alone.
    auto device = make<Device>();
    device->description.handle = virtual_gamepad->handle;
    device->virtual_device_owner = &client;
    m_devices.set(virtual_gamepad->handle, move(device));

    return virtual_gamepad;
}

void GamepadManager::set_virtual_gamepad_button(WebContentClient& client, Web::Gamepad::GamepadHandle handle, i32 button, bool down)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    m_source->set_virtual_gamepad_button(handle, button, down);
}

void GamepadManager::set_virtual_gamepad_axis(WebContentClient& client, Web::Gamepad::GamepadHandle handle, i32 axis, i16 value)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    m_source->set_virtual_gamepad_axis(handle, axis, value);
}

void GamepadManager::disconnect_virtual_gamepad(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    remove_device(handle);
    m_source->disconnect_virtual_gamepad(handle);
}

Vector<Web::Gamepad::GamepadChangeEvent> GamepadManager::pump_gamepad_events(WebContentClient& client)
{
    if (m_source_started)
        m_source->poll();

    auto consumer = m_consumers.get(&client);
    if (!consumer.has_value())
        return {};
    return move((*consumer)->buffered_virtual_gamepad_events);
}

// The renderer forgets only this page's use of gamepads, so the other pages of the process stay registered.
void GamepadManager::reset_access_for_testing(WebContentPage& page)
{
    auto consumer = m_consumers.get(&page.client());
    if (!consumer.has_value())
        return;
    (*consumer)->pages_using_gamepads.remove(page.id());
    (*consumer)->has_seen_gamepad_user_gesture = false;
    update_polling_state();
}

Vector<Web::Gamepad::ReceivedDualRumbleEffect> GamepadManager::virtual_gamepad_received_rumble_effects(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return {};
    return m_source->virtual_gamepad_received_rumble_effects(handle);
}

Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> GamepadManager::virtual_gamepad_received_trigger_rumble_effects(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return {};
    return m_source->virtual_gamepad_received_trigger_rumble_effects(handle);
}

void GamepadManager::gamepads_changed(Vector<Web::Gamepad::GamepadState> states)
{
    for (auto& state : states) {
        auto* device = device_for_handle(state.handle);
        if (!device || !device->announced)
            continue;
        device->last_published_state = move(state);
        for (auto& consumer_entry : m_consumers)
            announce_devices_after_gamepad_user_gesture(*consumer_entry.value, *device);
        publish_device_state(*device);
    }

    notify_consumers_of_state_changes();
}

void GamepadManager::notify_consumers_of_state_changes()
{
    for (auto& entry : m_consumers) {
        if (exchange(entry.value->has_pending_state_changes, false))
            entry.key->async_gamepad_states_changed();
    }
}

void GamepadManager::publish_device_state(Device const& device)
{
    auto contains_gamepad_user_gesture = device.contains_gamepad_user_gesture();

    for (auto& entry : m_consumers) {
        auto& consumer = *entry.value;
        if (auto slot_index = consumer.slot_index_for_device(device.description.handle); slot_index.has_value()) {
            Web::Gamepad::publish_gamepad_state_to_slot(consumer.shared_state_slot(*slot_index), device.last_published_state);
            consumer.has_pending_state_changes = true;

            // The activation is in place before the process learns of the state that gave it.
            if (contains_gamepad_user_gesture)
                consumer.notify_about_user_activation_for_gamepad_user_gesture();
        }
    }
}

// https://github.com/w3c/gamepad/pull/230
// The activation notification steps that updating a gamepad's state runs for each document it belongs to. The UI
// process runs them, as the gesture is input it received.
void GamepadManager::Consumer::notify_about_user_activation_for_gamepad_user_gesture() const
{
    HashTable<Web::HTML::CrossProcessId> notified_document_ids;
    client.for_each_page([&](WebContentPage& page) {
        page.for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& navigable) {
            // Only the documents allowed to use gamepads have them, so only those start the steps. The steps then reach
            // their ancestors and same-origin descendants whatever those may use.
            if (navigable.active_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention()
                && navigable.active_document().is_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad))
                navigable.run_activation_notification_steps_for_active_document(notified_document_ids);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
}

void GamepadManager::device_connected(Web::Gamepad::GamepadDescription description, Web::Gamepad::GamepadState state)
{
    auto* device = device_for_handle(description.handle);
    if (!device) {
        auto new_device = make<Device>();
        device = new_device.ptr();
        m_devices.set(description.handle, move(new_device));
    }
    if (device->announced)
        return;

    device->description = move(description);
    device->last_published_state = move(state);
    announce_device(*device);
}

void GamepadManager::announce_device(Device& device)
{
    if (device.announced)
        return;

    device.announced = true;

    for (auto& entry : m_consumers) {
        if (entry.value->may_receive_device(device))
            announce_device_to_consumer(device, *entry.value);
        else
            announce_devices_after_gamepad_user_gesture(*entry.value, device);
    }
}

// A process learns of the gamepads once the user interacts with one while a page of the process that uses them has the
// user's attention, so that it cannot fingerprint gamepads the user never used with it.
void GamepadManager::announce_devices_after_gamepad_user_gesture(Consumer& consumer, Device const& device)
{
    if (consumer.has_seen_gamepad_user_gesture)
        return;
    if (!device.may_be_announced_to(consumer.client) || !device.contains_gamepad_user_gesture())
        return;
    if (!consumer.has_page_that_may_receive_gamepads())
        return;

    consumer.has_seen_gamepad_user_gesture = true;
    fill_vacant_slots(consumer);
}

bool GamepadManager::Device::contains_gamepad_user_gesture() const
{
    return Web::Gamepad::contains_gamepad_user_gesture(description, last_published_state);
}

bool GamepadManager::Device::may_be_announced_to(WebContentClient const& client) const
{
    return !virtual_device_owner || virtual_device_owner == &client;
}

bool GamepadManager::Consumer::has_page_that_may_receive_gamepads() const
{
    return any_of(pages_using_gamepads, [&](Web::PageId page_id) {
        auto* page = client.page(page_id);
        if (!page)
            return false;

        auto may_receive_gamepads = false;
        page->for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& navigable) {
            may_receive_gamepads = navigable.active_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention()
                && navigable.active_document().is_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad);
            return may_receive_gamepads ? IterationDecision::Break : IterationDecision::Continue;
        });
        return may_receive_gamepads;
    });
}

bool GamepadManager::Consumer::may_receive_device(Device const& device) const
{
    return has_seen_gamepad_user_gesture && device.may_be_announced_to(client);
}

void GamepadManager::fill_vacant_slots(Consumer& consumer)
{
    for (auto& entry : m_devices) {
        if (!consumer.slot_handles.contains_slow(0))
            return;
        auto& device = *entry.value;
        if (!device.announced || consumer.slot_index_for_device(device.description.handle).has_value())
            continue;
        if (consumer.may_receive_device(device))
            announce_device_to_consumer(device, consumer);
    }
}

void GamepadManager::announce_device_to_consumer(Device const& device, Consumer& consumer)
{
    auto slot_index = consumer.allocate_shared_state_slot(device.description.handle);
    if (!slot_index.has_value()) {
        dbgln("All {} shared gamepad state slots are occupied; '{}' is not announced to this WebContent process", Web::Gamepad::MAX_SHARED_GAMEPADS, device.description.id);
        return;
    }
    Web::Gamepad::publish_gamepad_state_to_slot(consumer.shared_state_slot(*slot_index), device.last_published_state);
    consumer.has_pending_state_changes = true;

    if (device.virtual_device_owner)
        consumer.buffered_virtual_gamepad_events.append(Web::Gamepad::GamepadConnectedEvent { device.description });
    else
        consumer.client.async_gamepad_connected(device.description);
}

void GamepadManager::remove_device(Web::Gamepad::GamepadHandle handle)
{
    auto removed_device = m_devices.take(handle);
    if (!removed_device.has_value())
        return;
    auto& device = **removed_device;

    for (auto& entry : m_consumers) {
        auto& consumer = *entry.value;
        auto slot_index = consumer.slot_index_for_device(handle);
        if (!slot_index.has_value())
            continue;
        Web::Gamepad::clear_gamepad_state_slot(consumer.shared_state_slot(*slot_index));
        consumer.slot_handles[*slot_index] = 0;

        if (device.virtual_device_owner)
            consumer.buffered_virtual_gamepad_events.append(Web::Gamepad::GamepadDisconnectedEvent { handle });
        else
            consumer.client.async_gamepad_disconnected(handle);

        fill_vacant_slots(consumer);
    }
}

Optional<size_t> GamepadManager::Consumer::allocate_shared_state_slot(Web::Gamepad::GamepadHandle handle)
{
    auto slot_index = slot_handles.first_index_of(0);
    if (slot_index.has_value())
        slot_handles[*slot_index] = handle;
    return slot_index;
}

Optional<size_t> GamepadManager::Consumer::slot_index_for_device(Web::Gamepad::GamepadHandle handle) const
{
    return slot_handles.first_index_of(handle);
}

Web::Gamepad::SharedGamepadStateSlot& GamepadManager::Consumer::shared_state_slot(size_t slot_index)
{
    return reinterpret_cast<Web::Gamepad::SharedGamepadStateBuffer*>(state_buffer.data<void>())->slots[slot_index];
}

GamepadManager::Device* GamepadManager::device_for_handle(Web::Gamepad::GamepadHandle handle)
{
    if (auto device = m_devices.get(handle); device.has_value())
        return *device;
    return nullptr;
}

GamepadManager::Device* GamepadManager::virtual_device_owned_by_client(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = device_for_handle(handle);
    if (!device || device->virtual_device_owner != &client)
        return nullptr;
    return device;
}

GamepadManager::Device* GamepadManager::device_announced_to_client(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto consumer = m_consumers.get(&client);
    if (!consumer.has_value() || !(*consumer)->slot_index_for_device(handle).has_value())
        return nullptr;
    return device_for_handle(handle);
}

}
