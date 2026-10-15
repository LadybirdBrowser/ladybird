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
