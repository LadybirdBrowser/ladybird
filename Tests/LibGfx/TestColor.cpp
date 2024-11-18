/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibTest/TestCase.h>

TEST_CASE(color)
{
    for (u16 i = 0; i < 256; ++i) {
        auto const gray = Color(i, i, i);
        EXPECT_EQ(gray, gray.to_grayscale());
    }
}

TEST_CASE(all_green)
{
    EXPECT_EQ(Color(Color::NamedColor::Green), Color::from_lab(87.8185, -79.2711, 80.9946));
    EXPECT_EQ(Color(Color::NamedColor::Green), Color::from_xyz50(0.385152, 0.716887, 0.097081));
    EXPECT_EQ(Color(Color::NamedColor::Green), Color::from_xyz65(0.357584, 0.715169, 0.119195));
}

TEST_CASE(hsv)
{
    EXPECT_EQ(Color(51, 179, 51), Color::from_hsv(120, 0.714285714, .7));
    EXPECT_EQ(Color(51, 179, 51, 128), Color::from_hsv(120, 0.714285714, .7).with_opacity(0.5));
    EXPECT_EQ(Color(87, 128, 77), Color::from_hsv(108, 0.4, .5));
}
