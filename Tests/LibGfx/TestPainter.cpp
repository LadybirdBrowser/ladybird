/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibGfx/DeprecatedPainter.h>

TEST_CASE(draw_scaled_bitmap_with_transform)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 40, 30 }));
    bitmap->fill(Gfx::Color::White);
    Gfx::DeprecatedPainter painter(bitmap);

    auto source_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 1, 1 }));
    source_bitmap->fill(Gfx::Color::Black);

    auto dest_rect = source_bitmap->rect();
    auto source_rect = source_bitmap->rect().to_rounded<float>();

    // Identity transform: Lower left pixel is black, rest stays white.
    Gfx::AffineTransform transform;
    painter.draw_scaled_bitmap_with_transform(dest_rect, source_bitmap, source_rect, transform);
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < bitmap->width(); ++x) {
            if (x == 0 && y == 0)
                EXPECT_EQ(bitmap->get_pixel(x, y), Color::Black);
            else
                EXPECT_EQ(bitmap->get_pixel(x, y), Color::White);
        }
    }

    // Scale up 1x1 source bitmap 10x in x and 5x in y and paint at 10, 20. Should fill that rect:
    bitmap->fill(Gfx::Color::White);
    transform = transform.translate(10, 20).scale(10, 5);
    painter.draw_scaled_bitmap_with_transform(dest_rect, source_bitmap, source_rect, transform);
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < bitmap->width(); ++x) {
            if (x >= 10 && x < 10 + 10 && y >= 20 && y < 20 + 5)
                EXPECT_EQ(bitmap->get_pixel(x, y), Color::Black);
            else
                EXPECT_EQ(bitmap->get_pixel(x, y), Color::White);
        }
    }
}

TEST_CASE(draw_rect_rough_bounds)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 10, 10 }));
    Gfx::DeprecatedPainter painter(*bitmap);
    painter.draw_rect(Gfx::IntRect(0, 0, 1, 1), Color::Black, true);
    painter.draw_rect(Gfx::IntRect(9, 9, 1, 1), Color::Black, true);
}
