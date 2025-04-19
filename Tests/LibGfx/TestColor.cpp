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

TEST_CASE(from_rgb)
{
    EXPECT_EQ(Color(0x00, 0x00, 0xff), Color::from_rgb(0x000000ff));
    EXPECT_EQ(Color(0x00, 0xff, 0x00), Color::from_rgb(0x0000ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00), Color::from_rgb(0x00ff0000));

    EXPECT_EQ(Color(0x00, 0x00, 0xff), Color::from_rgb(0xff0000ff));
    EXPECT_EQ(Color(0x00, 0xff, 0x00), Color::from_rgb(0xff00ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00), Color::from_rgb(0xffff0000));

    EXPECT_EQ(Color(0xaa, 0xbb, 0xcc), Color::from_rgb(0x00aabbcc));
}

TEST_CASE(from_argb)
{
    EXPECT_EQ(Color(0x00, 0x00, 0x00, 0xff), Color::from_argb(0xff000000));
    EXPECT_EQ(Color(0x00, 0x00, 0xff, 0x00), Color::from_argb(0x000000ff));
    EXPECT_EQ(Color(0x00, 0xff, 0x00, 0x00), Color::from_argb(0x0000ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00, 0x00), Color::from_argb(0x00ff0000));

    EXPECT_EQ(Color(0xaa, 0xbb, 0xcc, 0xdd), Color::from_argb(0xddaabbcc));
}

TEST_CASE(from_bgr)
{
    EXPECT_EQ(Color(0x00, 0x00, 0xff), Color::from_bgr(0x00ff0000));
    EXPECT_EQ(Color(0x00, 0xff, 0x00), Color::from_bgr(0x0000ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00), Color::from_bgr(0x000000ff));

    EXPECT_EQ(Color(0x00, 0x00, 0xff), Color::from_bgr(0xffff0000));
    EXPECT_EQ(Color(0x00, 0xff, 0x00), Color::from_bgr(0xff00ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00), Color::from_bgr(0xff0000ff));

    EXPECT_EQ(Color(0xaa, 0xbb, 0xcc), Color::from_bgr(0x00ccbbaa));
}

TEST_CASE(from_abgr)
{
    EXPECT_EQ(Color(0x00, 0x00, 0x00, 0xff), Color::from_abgr(0xff000000));
    EXPECT_EQ(Color(0x00, 0x00, 0xff, 0x00), Color::from_abgr(0x00ff0000));
    EXPECT_EQ(Color(0x00, 0xff, 0x00, 0x00), Color::from_abgr(0x0000ff00));
    EXPECT_EQ(Color(0xff, 0x00, 0x00, 0x00), Color::from_abgr(0x000000ff));

    EXPECT_EQ(Color(0xaa, 0xbb, 0xcc, 0xdd), Color::from_abgr(0xddccbbaa));
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

TEST_CASE(hsl)
{
    EXPECT_EQ(Color(191, 191, 0), Color::from_hsl(-300, 1.0, 0.375));
    EXPECT_EQ(Color(159, 138, 96), Color::from_hsl(400, 0.25, 0.5));
    EXPECT_EQ(Color(159, 96, 128), Color::from_hsl(330, 0.25, 0.5));
    EXPECT_EQ(Color(128, 0, 128), Color::from_hsl(300, 1.0, 0.25));
    EXPECT_EQ(Color(0, 128, 128), Color::from_hsl(180, 1.0, 0.25));
    EXPECT_EQ(Color(128, 239, 16), Color::from_hsl(90, 0.875, 0.5));
    EXPECT_EQ(Color(128, 223, 32), Color::from_hsl(90, 0.75, 0.5));
    EXPECT_EQ(Color(128, 207, 48), Color::from_hsl(90, 0.625, 0.5));
    EXPECT_EQ(Color(128, 191, 64), Color::from_hsl(90, 0.5, 0.5));
    EXPECT_EQ(Color(128, 175, 80), Color::from_hsl(90, 0.375, 0.5));
    EXPECT_EQ(Color(128, 159, 96), Color::from_hsl(90, 0.25, 0.5));
}
