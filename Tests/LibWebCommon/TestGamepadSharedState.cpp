/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>

using namespace Web::Gamepad;

TEST_CASE(untouched_slot_reads_as_unchanged)
{
    SharedGamepadStateSlot slot {};
    u32 last_observed_sequence = 0;

    EXPECT(!read_gamepad_state_from_slot(slot, last_observed_sequence).has_value());
    EXPECT_EQ(last_observed_sequence, 0u);
}

TEST_CASE(published_state_is_read_once)
{
    SharedGamepadStateSlot slot {};
    u32 last_observed_sequence = 0;

    GamepadState state { 7, { -32768, 0, 32767 }, { 0, 1, 32767 } };
    publish_gamepad_state_to_slot(slot, state);

    auto read_state = read_gamepad_state_from_slot(slot, last_observed_sequence);
    EXPECT(read_state.has_value());
    EXPECT_EQ(*read_state, state);
    EXPECT_EQ(last_observed_sequence, 2u);

    EXPECT(!read_gamepad_state_from_slot(slot, last_observed_sequence).has_value());
    EXPECT_EQ(last_observed_sequence, 2u);

    GamepadState changed_state { 7, { 0, 0, 0 }, { 1, 1, 0 } };
    publish_gamepad_state_to_slot(slot, changed_state);

    read_state = read_gamepad_state_from_slot(slot, last_observed_sequence);
    EXPECT(read_state.has_value());
    EXPECT_EQ(*read_state, changed_state);
    EXPECT_EQ(last_observed_sequence, 4u);
}

TEST_CASE(cleared_slot_reads_as_unoccupied)
{
    SharedGamepadStateSlot slot {};
    u32 last_observed_sequence = 0;

    GamepadState state { 3, { 100 }, { 1 } };
    publish_gamepad_state_to_slot(slot, state);
    clear_gamepad_state_slot(slot);

    EXPECT(!read_gamepad_state_from_slot(slot, last_observed_sequence).has_value());
    EXPECT_EQ(last_observed_sequence, 4u);

    GamepadState state_of_next_device { 4, { -100 }, { 0 } };
    publish_gamepad_state_to_slot(slot, state_of_next_device);

    auto read_state = read_gamepad_state_from_slot(slot, last_observed_sequence);
    EXPECT(read_state.has_value());
    EXPECT_EQ(*read_state, state_of_next_device);
    EXPECT_EQ(last_observed_sequence, 6u);
}

TEST_CASE(oversized_input_lists_are_truncated)
{
    SharedGamepadStateSlot slot {};
    u32 last_observed_sequence = 0;

    GamepadState state;
    state.handle = 1;
    for (size_t axis_index = 0; axis_index < MAX_SHARED_GAMEPAD_AXES + 4; ++axis_index)
        state.axis_values.append(static_cast<i16>(axis_index));
    for (size_t button_index = 0; button_index < MAX_SHARED_GAMEPAD_BUTTONS + 8; ++button_index)
        state.button_values.append(static_cast<i16>(button_index));
    publish_gamepad_state_to_slot(slot, state);

    auto read_state = read_gamepad_state_from_slot(slot, last_observed_sequence);
    EXPECT(read_state.has_value());
    EXPECT_EQ(read_state->axis_values.size(), MAX_SHARED_GAMEPAD_AXES);
    EXPECT_EQ(read_state->button_values.size(), MAX_SHARED_GAMEPAD_BUTTONS);
    EXPECT_EQ(read_state->axis_values.last(), static_cast<i16>(MAX_SHARED_GAMEPAD_AXES - 1));
    EXPECT_EQ(read_state->button_values.last(), static_cast<i16>(MAX_SHARED_GAMEPAD_BUTTONS - 1));
}

TEST_CASE(slot_mid_write_is_not_read)
{
    SharedGamepadStateSlot slot {};
    u32 last_observed_sequence = 0;

    GamepadState state { 5, { 1, 2 }, { 1 } };
    publish_gamepad_state_to_slot(slot, state);

    slot.sequence = 3;
    EXPECT(!read_gamepad_state_from_slot(slot, last_observed_sequence).has_value());
    EXPECT_EQ(last_observed_sequence, 0u);

    slot.sequence = 4;
    auto read_state = read_gamepad_state_from_slot(slot, last_observed_sequence);
    EXPECT(read_state.has_value());
    EXPECT_EQ(*read_state, state);
    EXPECT_EQ(last_observed_sequence, 4u);
}
