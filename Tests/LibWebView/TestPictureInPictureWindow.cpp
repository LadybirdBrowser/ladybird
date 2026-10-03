/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/PictureInPictureWindow.h>

using WebView::PictureInPictureWindow;

static constexpr Gfx::IntSize screen_size { 1920, 1080 };
static constexpr Gfx::IntSize landscape_video { 1920, 1080 };
static constexpr Gfx::IntSize portrait_video { 1080, 1920 };
static constexpr Gfx::IntSize wide_video { 3200, 900 };

TEST_CASE(window_opens_at_a_quarter_of_the_screen_width)
{
    EXPECT_EQ(PictureInPictureWindow::initial_size(landscape_video, screen_size), Gfx::IntSize(480, 270));
}

TEST_CASE(window_opens_no_taller_than_half_of_the_screen)
{
    EXPECT_EQ(PictureInPictureWindow::initial_size(portrait_video, screen_size), Gfx::IntSize(303, 540));
}

TEST_CASE(minimum_size_keeps_the_aspect_ratio)
{
    EXPECT_EQ(PictureInPictureWindow::minimum_size(landscape_video), Gfx::IntSize(240, 135));
    EXPECT_EQ(PictureInPictureWindow::minimum_size(portrait_video), Gfx::IntSize(240, 426));
}

TEST_CASE(minimum_size_of_a_wide_video_keeps_a_minimum_height)
{
    EXPECT_EQ(PictureInPictureWindow::minimum_size(wide_video), Gfx::IntSize(355, 100));
}

TEST_CASE(maximum_size_is_half_of_the_screen)
{
    EXPECT_EQ(PictureInPictureWindow::maximum_size(landscape_video, screen_size), Gfx::IntSize(960, 540));
    EXPECT_EQ(PictureInPictureWindow::maximum_size(portrait_video, screen_size), Gfx::IntSize(303, 540));
}

TEST_CASE(window_keeps_its_width_when_its_video_changes_shape)
{
    EXPECT_EQ(PictureInPictureWindow::size_for_video_size({ 480, 270 }, { 640, 480 }, screen_size), Gfx::IntSize(480, 360));
}

TEST_CASE(window_stays_within_its_limits_when_its_video_changes_shape)
{
    EXPECT_EQ(PictureInPictureWindow::size_for_video_size({ 240, 180 }, wide_video, screen_size), Gfx::IntSize(355, 100));
    EXPECT_EQ(PictureInPictureWindow::size_for_video_size({ 900, 506 }, portrait_video, screen_size), Gfx::IntSize(303, 540));
}

TEST_CASE(window_without_a_video_size_is_sixteen_by_nine)
{
    EXPECT_EQ(PictureInPictureWindow::minimum_size({}), Gfx::IntSize(240, 135));
}
