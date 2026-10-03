/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/TearableAtomic.h>
#include <AK/Types.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>

namespace Web::Gamepad {

inline constexpr size_t MAX_SHARED_GAMEPADS = 8;
inline constexpr size_t MAX_SHARED_GAMEPAD_AXES = 8;
inline constexpr size_t MAX_SHARED_GAMEPAD_BUTTONS = 32;

struct SharedGamepadState {
    GamepadHandle handle;
    u32 axis_count;
    u32 button_count;
    Array<i16, MAX_SHARED_GAMEPAD_AXES> axis_values;
    Array<i16, MAX_SHARED_GAMEPAD_BUTTONS> button_values;
};

// A slot's sequence number is odd while the UI process is writing it.
struct SharedGamepadStateSlot {
    u32 sequence;
    TearableAtomic<SharedGamepadState> state;
};

struct SharedGamepadStateBuffer {
    Array<SharedGamepadStateSlot, MAX_SHARED_GAMEPADS> slots;
};

WEBCOMMON_API void publish_gamepad_state_to_slot(SharedGamepadStateSlot&, GamepadState const&);
WEBCOMMON_API void clear_gamepad_state_slot(SharedGamepadStateSlot&);
WEBCOMMON_API Optional<GamepadState> read_gamepad_state_from_slot(SharedGamepadStateSlot const&, u32& last_observed_sequence);

}
