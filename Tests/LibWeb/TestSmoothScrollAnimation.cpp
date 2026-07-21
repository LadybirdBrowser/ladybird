/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/SmoothScrollAnimation.h>

using Web::Compositor::SmoothScrollAnimation;

TEST_CASE(zero_distance_completes_immediately)
{
    SmoothScrollAnimation animation({ 20, 40 }, { 20, 40 }, 1.0);

    EXPECT_EQ(animation.duration(), AK::Duration::zero());
    auto sample = animation.sample(AK::Duration::zero());
    EXPECT(sample.complete);
    EXPECT_EQ(sample.offset, Gfx::FloatPoint(20, 40));
}

TEST_CASE(duration_is_based_on_distance_and_capped)
{
    SmoothScrollAnimation short_animation({ 0, 0 }, { 60, 80 }, 1.0);
    EXPECT_EQ(short_animation.duration(), AK::Duration::from_milliseconds(100));

    SmoothScrollAnimation long_animation({ 0, 0 }, { 1000, 1000 }, 1.0);
    EXPECT_EQ(long_animation.duration(), AK::Duration::from_milliseconds(200));
}

TEST_CASE(samples_an_ease_in_out_curve)
{
    SmoothScrollAnimation animation({ 10, 20 }, { 110, 220 }, 1.0);

    auto start = animation.sample(AK::Duration::zero());
    EXPECT(!start.complete);
    EXPECT_EQ(start.offset, Gfx::FloatPoint(10, 20));

    auto midpoint = animation.sample(AK::Duration::from_milliseconds(animation.duration().to_milliseconds() / 2));
    EXPECT(!midpoint.complete);
    EXPECT_APPROXIMATE(midpoint.offset.x(), 60.0f);
    EXPECT_APPROXIMATE(midpoint.offset.y(), 120.0f);

    auto end = animation.sample(animation.duration());
    EXPECT(end.complete);
    EXPECT_EQ(end.offset, Gfx::FloatPoint(110, 220));
}

TEST_CASE(duration_is_independent_of_device_scale)
{
    SmoothScrollAnimation css_pixel_animation({ 0, 0 }, { 60, 80 }, 1.0);
    SmoothScrollAnimation device_pixel_animation({ 0, 0 }, { 120, 160 }, 2.0);

    EXPECT_EQ(css_pixel_animation.duration(), device_pixel_animation.duration());
}
