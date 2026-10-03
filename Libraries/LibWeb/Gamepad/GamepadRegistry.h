/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebCommon/Gamepad/GamepadStateBuffer.h>

namespace Web::Gamepad {

class WEB_API GamepadRegistry {
public:
    static GamepadRegistry& the();

    void gamepad_connected(GamepadDescription const&);
    void gamepad_disconnected(GamepadHandle);

    void set_shared_state_buffer(GamepadStateBuffer);
    Vector<GamepadState> take_changed_shared_states();

    void for_each_connected_gamepad(Function<void(GamepadDescription const&)>) const;

private:
    OrderedHashMap<GamepadHandle, GamepadDescription> m_descriptions;
    HashMap<GamepadHandle, GamepadState> m_latest_states;
    GamepadStateBuffer m_shared_state_buffer;
    Array<u32, MAX_SHARED_GAMEPADS> m_last_observed_slot_sequences {};
};

}
