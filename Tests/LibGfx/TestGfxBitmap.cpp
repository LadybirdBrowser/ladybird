/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>

TEST_CASE(0001_bitmap_upscaling_width1_height1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 1, 1 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(5.5f, 5.5f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(6, 6));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0002_bitmap_upscaling_width1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 1, 10 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(5.5f, 5.5f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(6, 55));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0003_bitmap_upscaling_height1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 10, 1 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(5.5f, 5.5f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(55, 6));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0004_bitmap_upscaling_keep_width)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 1, 10 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(1.f, 5.5f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(1, 55));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0005_bitmap_upscaling_keep_height)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 10, 1 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(5.5f, 1.f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(55, 1));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0006_bitmap_downscaling_width1_height1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 10, 10 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(0.099f, 0.099f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(1, 1));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0007_bitmap_downscaling_width1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 10, 10 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(1.f, 0.099f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(10, 1));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}

TEST_CASE(0008_bitmap_downscaling_height1)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, Gfx::IntSize { 10, 10 });
    EXPECT_EQ(bitmap.is_error(), false);
    bitmap.value()->fill(Gfx::Color::White);
    auto scaledBitmap = bitmap.value()->scaled(0.099f, 1.f);
    EXPECT_EQ(scaledBitmap.is_error(), false);
    EXPECT_EQ(scaledBitmap.value()->size(), Gfx::IntSize(1, 10));
    for (auto x = 0; x < scaledBitmap.value()->width(); x++) {
        for (auto y = 0; y < scaledBitmap.value()->height(); y++) {
            EXPECT_EQ(scaledBitmap.value()->get_pixel(x, y), bitmap.value()->get_pixel(0, 0));
        }
    }
}
