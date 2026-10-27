/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PictureInPictureControls.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

TEST_CASE(buttons_sit_in_the_top_corners)
{
    auto geometry = picture_in_picture_controls_geometry({ 800, 600 }, 1.0);

    EXPECT_EQ(geometry.back_to_tab_button, (Gfx::IntRect { 762, 10, 28, 28 }));
    EXPECT_EQ(geometry.close_button, (Gfx::IntRect { 10, 10, 28, 28 }));
}

TEST_CASE(geometry_scales_with_device_pixel_ratio)
{
    auto geometry = picture_in_picture_controls_geometry({ 1600, 1200 }, 2.0);

    EXPECT_EQ(geometry.back_to_tab_button, (Gfx::IntRect { 1524, 20, 56, 56 }));
    EXPECT_EQ(geometry.close_button, (Gfx::IntRect { 20, 20, 56, 56 }));
}

TEST_CASE(controls_are_round)
{
    auto geometry = picture_in_picture_controls_geometry({ 800, 600 }, 1.0);
    auto buttons = geometry.buttons();

    EXPECT(overlay_action_at(buttons, geometry.back_to_tab_button.center()) == PictureInPictureControl::BackToTab);
    EXPECT(overlay_action_at(buttons, geometry.close_button.center()) == PictureInPictureControl::Close);
    EXPECT(!overlay_action_at(buttons, geometry.back_to_tab_button.top_left()).has_value());
    EXPECT(!overlay_action_at(buttons, { 790, 10 }).has_value());
    EXPECT(!overlay_action_at(buttons, { 400, 22 }).has_value());
}

TEST_CASE(control_values_are_validated)
{
    EXPECT(picture_in_picture_control_from_underlying(to_underlying(PictureInPictureControl::BackToTab)) == PictureInPictureControl::BackToTab);
    EXPECT(picture_in_picture_control_from_underlying(to_underlying(PictureInPictureControl::Close)) == PictureInPictureControl::Close);
    EXPECT(!picture_in_picture_control_from_underlying(255).has_value());
}
