/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PausedDebuggerOverlay.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

TEST_CASE(buttons_are_centered_with_the_toolbar)
{
    auto geometry = paused_debugger_overlay_geometry({ 500, 300 }, 1.0);

    EXPECT_EQ(geometry.toolbar, (Gfx::IntRect { 125, 30, 250, 38 }));
    EXPECT_EQ(geometry.step_over_button, (Gfx::IntRect { 295, 30, 40, 38 }));
    EXPECT_EQ(geometry.continue_button, (Gfx::IntRect { 335, 30, 40, 38 }));
}

TEST_CASE(geometry_scales_with_device_pixel_ratio)
{
    auto geometry = paused_debugger_overlay_geometry({ 1000, 600 }, 2.0);

    EXPECT_EQ(geometry.toolbar, (Gfx::IntRect { 250, 60, 500, 76 }));
    EXPECT_EQ(geometry.step_over_button, (Gfx::IntRect { 590, 60, 80, 76 }));
    EXPECT_EQ(geometry.continue_button, (Gfx::IntRect { 670, 60, 80, 76 }));
}

TEST_CASE(actions_only_match_their_buttons)
{
    auto geometry = paused_debugger_overlay_geometry({ 500, 300 }, 1.0);
    auto buttons = geometry.buttons();

    auto step_over_action = overlay_action_at(buttons, geometry.step_over_button.center());
    EXPECT(step_over_action.has_value());
    EXPECT(*step_over_action == PausedDebuggerOverlayAction::StepOver);

    auto continue_action = overlay_action_at(buttons, geometry.continue_button.center());
    EXPECT(continue_action.has_value());
    EXPECT(*continue_action == PausedDebuggerOverlayAction::Continue);
    EXPECT(!overlay_action_at(buttons, geometry.message.center()).has_value());
    EXPECT(!overlay_action_at(buttons, { 0, 0 }).has_value());
}

TEST_CASE(toolbar_stays_inside_a_small_viewport)
{
    auto geometry = paused_debugger_overlay_geometry({ 70, 25 }, 1.0);

    EXPECT_EQ(geometry.toolbar, (Gfx::IntRect { 0, 0, 70, 25 }));
    EXPECT_EQ(geometry.step_over_button.left(), 0);
    EXPECT_EQ(geometry.continue_button.right(), 70);
}

TEST_CASE(action_values_are_validated)
{
    auto step_over = paused_debugger_overlay_action_from_underlying(to_underlying(PausedDebuggerOverlayAction::StepOver));
    EXPECT(step_over == PausedDebuggerOverlayAction::StepOver);

    auto continue_action = paused_debugger_overlay_action_from_underlying(to_underlying(PausedDebuggerOverlayAction::Continue));
    EXPECT(continue_action == PausedDebuggerOverlayAction::Continue);
    EXPECT(!paused_debugger_overlay_action_from_underlying(255).has_value());
}
