/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/OverlayGeometry.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

TEST_CASE(overlay_lengths_round_to_device_pixels)
{
    EXPECT_EQ(scaled_overlay_length(38, 1.25), 48);
    EXPECT_EQ(scaled_overlay_length(30, 1.25), 38);
}

TEST_CASE(overlay_lengths_are_at_least_one_device_pixel)
{
    EXPECT_EQ(scaled_overlay_length(1, 0.25), 1);
}

enum class TestAction : u8 {
    First,
    Second,
};

TEST_CASE(a_round_button_leaves_the_corners_of_its_bounds)
{
    OverlayButton<TestAction> round_button { .action = TestAction::First, .bounds = { 8, 8, 28, 28 }, .shape = OverlayButtonShape::Circle };
    EXPECT(round_button.contains({ 22, 22 }));
    EXPECT(round_button.contains({ 8, 22 }));
    EXPECT(round_button.contains({ 22, 35 }));
    EXPECT(!round_button.contains({ 31, 32 }));
    EXPECT(!round_button.contains({ 32, 31 }));
    EXPECT(!round_button.contains({ 8, 8 }));
    EXPECT(!round_button.contains({ 34, 10 }));

    OverlayButton<TestAction> rectangular_button { .action = TestAction::First, .bounds = { 8, 8, 28, 28 }, .shape = OverlayButtonShape::Rectangle };
    EXPECT(rectangular_button.contains({ 8, 8 }));
    EXPECT(!rectangular_button.contains({ 36, 36 }));
}

TEST_CASE(buttons_are_found_by_position_and_bounded_together)
{
    Array<OverlayButton<TestAction>, 2> buttons {
        OverlayButton<TestAction> { .action = TestAction::First, .bounds = { 0, 0, 10, 10 }, .shape = OverlayButtonShape::Rectangle },
        OverlayButton<TestAction> { .action = TestAction::Second, .bounds = { 90, 20, 10, 10 }, .shape = OverlayButtonShape::Rectangle },
    };
    EXPECT(overlay_action_at(buttons, { 5, 5 }) == TestAction::First);
    EXPECT(overlay_action_at(buttons, { 95, 25 }) == TestAction::Second);
    EXPECT(!overlay_action_at(buttons, { 50, 15 }).has_value());
    EXPECT_EQ(overlay_buttons_bounds(buttons), (Gfx::IntRect { 0, 0, 100, 30 }));
}
