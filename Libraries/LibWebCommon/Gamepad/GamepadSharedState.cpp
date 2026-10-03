/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>

namespace Web::Gamepad {

void publish_gamepad_state_to_slot(SharedGamepadStateSlot& slot, GamepadState const& state)
{
    SharedGamepadState shared_state {};
    shared_state.handle = state.handle;
    shared_state.axis_count = min(state.axis_values.size(), MAX_SHARED_GAMEPAD_AXES);
    shared_state.button_count = min(state.button_values.size(), MAX_SHARED_GAMEPAD_BUTTONS);
    for (size_t axis_index = 0; axis_index < shared_state.axis_count; ++axis_index)
        shared_state.axis_values[axis_index] = state.axis_values[axis_index];
    for (size_t button_index = 0; button_index < shared_state.button_count; ++button_index)
        shared_state.button_values[button_index] = state.button_values[button_index];

    auto sequence = AK::atomic_load(&slot.sequence, AK::MemoryOrder::memory_order_relaxed);

    // The release fence keeps the payload writes from becoming visible before the odd sequence number.
    AK::atomic_store(&slot.sequence, sequence + 1, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_release);

    slot.state.store_relaxed(shared_state);

    AK::atomic_store(&slot.sequence, sequence + 2, AK::MemoryOrder::memory_order_release);
}

void clear_gamepad_state_slot(SharedGamepadStateSlot& slot)
{
    publish_gamepad_state_to_slot(slot, GamepadState {});
}

Optional<GamepadState> read_gamepad_state_from_slot(SharedGamepadStateSlot const& slot, u32& last_observed_sequence)
{
    static constexpr int MAX_READ_ATTEMPTS = 10;

    for (int attempt = 0; attempt < MAX_READ_ATTEMPTS; ++attempt) {
        auto sequence_before = AK::atomic_load(&slot.sequence, AK::MemoryOrder::memory_order_acquire);
        if (sequence_before == last_observed_sequence)
            return {};
        if (sequence_before % 2 == 1)
            continue;

        auto shared_state = slot.state.load_relaxed();

        // The acquire fence keeps the payload reads from being reordered past the sequence number recheck.
        AK::atomic_thread_fence(AK::MemoryOrder::memory_order_acquire);
        auto sequence_after = AK::atomic_load(&slot.sequence, AK::MemoryOrder::memory_order_relaxed);
        if (sequence_before != sequence_after)
            continue;

        last_observed_sequence = sequence_before;
        if (shared_state.handle == 0)
            return {};

        GamepadState state;
        state.handle = shared_state.handle;
        state.axis_values.append(shared_state.axis_values.data(), min(static_cast<size_t>(shared_state.axis_count), MAX_SHARED_GAMEPAD_AXES));
        state.button_values.append(shared_state.button_values.data(), min(static_cast<size_t>(shared_state.button_count), MAX_SHARED_GAMEPAD_BUTTONS));
        return state;
    }

    return {};
}

}
