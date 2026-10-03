/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/Gamepad/GamepadRegistry.h>

namespace Web::Gamepad {

GamepadRegistry& GamepadRegistry::the()
{
    static NeverDestroyed<GamepadRegistry> s_the;
    return *s_the;
}

void GamepadRegistry::gamepad_connected(GamepadDescription const& description)
{
    m_descriptions.set(description.handle, description);
}

void GamepadRegistry::gamepad_disconnected(GamepadHandle handle)
{
    m_descriptions.remove(handle);
    m_latest_states.remove(handle);
}

void GamepadRegistry::set_shared_state_buffer(GamepadStateBuffer shared_state_buffer)
{
    if (!shared_state_buffer.is_valid())
        return;
    m_shared_state_buffer = move(shared_state_buffer);
}

Vector<GamepadState> GamepadRegistry::take_changed_shared_states()
{
    if (!m_shared_state_buffer.is_valid())
        return {};

    Vector<GamepadState> changed_states;
    auto const& slots = m_shared_state_buffer.data()->slots;
    for (size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
        auto state = read_gamepad_state_from_slot(slots[slot_index], m_last_observed_slot_sequences[slot_index]);
        if (!state.has_value())
            continue;
        if (auto latest_state = m_latest_states.get(state->handle); latest_state.has_value() && *latest_state == *state)
            continue;
        m_latest_states.set(state->handle, *state);
        changed_states.append(state.release_value());
    }

    return changed_states;
}

void GamepadRegistry::for_each_connected_gamepad(Function<void(GamepadDescription const&)> callback) const
{
    for (auto const& entry : m_descriptions)
        callback(entry.value);
}

}
