/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/SmoothScrollAnimation.h>

using Web::Compositor::ScrollAnimationKind;
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

TEST_CASE(momentum_travels_for_as_long_as_its_distance_takes)
{
    // Momentum covers a longer distance over more of its decaying frames rather than at a greater speed.
    SmoothScrollAnimation short_animation({ 0, 0 }, { 0, 200 }, 1.0, ScrollAnimationKind::Momentum);
    SmoothScrollAnimation long_animation({ 0, 0 }, { 0, 1000 }, 1.0, ScrollAnimationKind::Momentum);

    EXPECT(short_animation.duration() > AK::Duration::from_milliseconds(200));
    EXPECT(long_animation.duration() > short_animation.duration());

    // However fast the momentum was, the scroll that replaces it still comes to rest.
    SmoothScrollAnimation enormous_animation({ 0, 0 }, { 0, 100'000'000 }, 1.0, ScrollAnimationKind::Momentum);
    EXPECT(enormous_animation.duration() <= AK::Duration::from_seconds(5));
}

TEST_CASE(momentum_decays_towards_its_destination)
{
    SmoothScrollAnimation animation({ 0, 0 }, { 0, 600 }, 1.0, ScrollAnimationKind::Momentum);

    auto start = animation.sample(AK::Duration::zero());
    EXPECT(!start.complete);
    EXPECT_EQ(start.offset, Gfx::FloatPoint(0, 0));

    // The distance covered by each frame shrinks, so more than half the way is covered in the first half of the time.
    auto midpoint = animation.sample(AK::Duration::from_milliseconds(animation.duration().to_milliseconds() / 2));
    EXPECT(!midpoint.complete);
    EXPECT(midpoint.offset.y() > 300);

    // The scroll never travels backwards on its way to its destination.
    auto previous_offset = 0.0f;
    for (auto elapsed = AK::Duration::zero(); elapsed < animation.duration(); elapsed = elapsed + AK::Duration::from_milliseconds(16)) {
        auto offset = animation.sample(elapsed).offset.y();
        EXPECT(offset >= previous_offset);
        previous_offset = offset;
    }

    auto end = animation.sample(animation.duration());
    EXPECT(end.complete);
    EXPECT_EQ(end.offset, Gfx::FloatPoint(0, 600));
}
