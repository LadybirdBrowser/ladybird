/*
 * Copyright (c) 2023, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibTest/TestCase.h>

// Scaling modes which use linear interpolation should use premultiplied alpha.
// This prevents colors from changing hue unexpectedly when there is a change in opacity.
// This test uses an image that transitions from a completely opaque pixel in the top left to a completely transparent background.
// We ensure that premultipled alpha is used by checking that the RGB values of the interpolated pixels do not change, just the alpha values.
TEST_CASE(test_painter_scaling_uses_premultiplied_alpha)
{
    auto test_scaling_mode = [](auto scaling_mode) {
        auto src_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 2 }));
        src_bitmap->fill(Color::Transparent);
        src_bitmap->set_pixel({ 0, 0 }, Color::White);

        auto scaled_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 5, 5 }));
        scaled_bitmap->fill(Color::Transparent);

        Gfx::Painter painter(scaled_bitmap);
        painter.draw_scaled_bitmap(scaled_bitmap->rect(), src_bitmap, src_bitmap->rect(), 1.0f, scaling_mode);

        auto top_left_pixel = scaled_bitmap->get_pixel(0, 0);
        EXPECT_EQ(top_left_pixel, Color::White);

        auto center_pixel = scaled_bitmap->get_pixel(scaled_bitmap->rect().center());
        EXPECT(center_pixel.alpha() > 0);
        EXPECT(center_pixel.alpha() < 255);
        EXPECT_EQ(center_pixel.with_alpha(0), Color(Color::White).with_alpha(0));

        auto bottom_right_pixel = scaled_bitmap->get_pixel(scaled_bitmap->rect().bottom_right().translated(-1));
        EXPECT_EQ(bottom_right_pixel, Color::Transparent);
    };

    test_scaling_mode(Gfx::ScalingMode::BilinearBlend);
    // FIXME: Include ScalingMode::SmoothPixels as part of this test
    //        This mode does not currently pass this test, as it  behave according to the spec
    //        defined here: https://drafts.csswg.org/css-images/#valdef-image-rendering-pixelated
    // test_scaling_mode(Gfx::ScalingMode::SmoothPixels);
}

TEST_CASE(test_bitmap_scaling_uses_premultiplied_alpha)
{
    auto src_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 2 }));
    src_bitmap->fill(Color::Transparent);
    src_bitmap->set_pixel({ 0, 0 }, Color::White);

    auto scaled_bitmap = MUST(src_bitmap->scaled(2.5f, 2.5f));
    EXPECT_EQ(scaled_bitmap->width(), 5);
    EXPECT_EQ(scaled_bitmap->height(), 5);
    auto top_left_pixel = scaled_bitmap->get_pixel(0, 0);
    EXPECT_EQ(top_left_pixel, Color::White);

    auto center_pixel = scaled_bitmap->get_pixel(scaled_bitmap->rect().center());
    EXPECT(center_pixel.alpha() > 0);
    EXPECT(center_pixel.alpha() < 255);
    EXPECT_EQ(center_pixel.with_alpha(0), Color(Color::White).with_alpha(0));

    auto bottom_right_pixel = scaled_bitmap->get_pixel(scaled_bitmap->rect().bottom_right().translated(-1));
    EXPECT_EQ(bottom_right_pixel, Color::Transparent);
}
