/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/VisualAnimation.h>

using namespace Web::Compositor;

static VisualAnimationEasing linear_easing()
{
    return {};
}

TEST_CASE(samples_opacity_across_iterations)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 250,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_APPROXIMATE(animation.sample(AK::Duration::zero())->opacity, 0.25f);
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(500))->opacity, 0.75f);
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(1000))->opacity, 0.25f);
}

TEST_CASE(clamps_overshooting_opacity_easing)
{
    VisualAnimationEasing overshooting_easing {
        .kind = VisualAnimationEasing::Kind::CubicBezier,
        .linear_points = {},
        .x1 = 0.5,
        .y1 = -1,
        .x2 = 0.5,
        .y2 = 2,
    };
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 100,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, overshooting_easing, 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_EQ(animation.sample(AK::Duration::zero())->opacity, 0.0f);
    animation.local_time_at_anchor_ms = 900;
    EXPECT_EQ(animation.sample(AK::Duration::zero())->opacity, 1.0f);
}

TEST_CASE(applies_playback_direction_and_keyframe_easing)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 250,
        .iteration_duration_ms = 1000,
        .playback_direction = VisualAnimationPlaybackDirection::AlternateReverse,
        .easing = linear_easing(),
        .keyframes = {
            { 0, VisualAnimationEasing { .kind = VisualAnimationEasing::Kind::Steps, .linear_points = {}, .interval_count = 4, .step_position = 1 }, 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_APPROXIMATE(animation.sample(AK::Duration::zero())->opacity, 0.75f);
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(1000))->opacity, 0.25f);
}

TEST_CASE(clamps_finite_animation_to_end_progress)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 900,
        .iteration_duration_ms = 1000,
        .iteration_count = 1,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(200))->opacity, 1.0f);
    animation.playback_direction = VisualAnimationPlaybackDirection::Reverse;
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(200))->opacity, 0.0f);
    animation.iteration_start = 0.5;
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(200))->opacity, 0.5f);
}

TEST_CASE(uses_following_interval_at_keyframe_boundary)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 500,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 0.5, VisualAnimationEasing { .kind = VisualAnimationEasing::Kind::Steps, .linear_points = {}, .interval_count = 4, .step_position = 0 }, 0.5f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_APPROXIMATE(animation.sample(AK::Duration::zero())->opacity, 0.625f);
}

TEST_CASE(interpolates_transform_operations_before_composing)
{
    VisualAnimationTransformList from {
        { VisualAnimationTransformOperationKind::TranslateX, { 0 } },
        { VisualAnimationTransformOperationKind::Rotate, { 0 } },
    };
    VisualAnimationTransformList to {
        { VisualAnimationTransformOperationKind::TranslateX, { 100 } },
        { VisualAnimationTransformOperationKind::Rotate, { AK::Pi<float> } },
    };
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Transform,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 500,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), move(from) },
            { 1, linear_easing(), move(to) },
        },
    };

    auto expected = Gfx::translation_matrix(Gfx::FloatVector3 { 50, 0, 0 })
        * Gfx::rotation_matrix(Gfx::FloatVector3 { 0, 0, 1 }, AK::Pi<float> / 2);
    auto sampled = animation.sample(AK::Duration::zero())->transform;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            auto sampled_value = sampled[row, column];
            auto expected_value = expected[row, column];
            EXPECT_APPROXIMATE(sampled_value, expected_value);
        }
    }
}

TEST_CASE(interpolates_background_color_in_premultiplied_srgb)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::BackgroundColor,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 500,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), Gfx::Color(255, 0, 0, 0) },
            { 1, linear_easing(), Gfx::Color(0, 0, 255, 255) },
        },
    };

    auto sampled = animation.sample(AK::Duration::zero())->background_color;
    EXPECT(sampled.has_value());
    EXPECT_EQ(sampled->red(), 0);
    EXPECT_EQ(sampled->green(), 0);
    EXPECT_EQ(sampled->blue(), 255);
    EXPECT_EQ(sampled->alpha(), 128);
}

TEST_CASE(extrapolates_overshooting_background_color_easing)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::BackgroundColor,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 750,
        .iteration_duration_ms = 1000,
        .easing = {
            .kind = VisualAnimationEasing::Kind::Linear,
            .linear_points = { { 0, 0 }, { 1, 2 } },
        },
        .keyframes = {
            { 0, linear_easing(), Gfx::Color(100, 20, 30) },
            { 1, linear_easing(), Gfx::Color(140, 40, 50) },
        },
    };

    auto sampled = animation.sample(AK::Duration::zero())->background_color;
    EXPECT(sampled.has_value());
    EXPECT_EQ(sampled->red(), 160);
    EXPECT_EQ(sampled->green(), 50);
    EXPECT_EQ(sampled->blue(), 60);
    EXPECT_EQ(sampled->alpha(), 255);
}

TEST_CASE(interpolates_filter_operation_initial_values)
{
    VisualAnimationFilterOperation drop_shadow {
        .kind = VisualAnimationFilterOperationKind::DropShadow,
        .amount = 10,
        .offset_x = 20,
        .offset_y = -10,
        .color = Gfx::Color::Blue,
    };
    auto interpolated_drop_shadow = drop_shadow.initial_value().interpolated_with(drop_shadow, 0.5);
    EXPECT_EQ(interpolated_drop_shadow.amount, 5);
    EXPECT_EQ(interpolated_drop_shadow.offset_x, 10);
    EXPECT_EQ(interpolated_drop_shadow.offset_y, -5);
    EXPECT_EQ(interpolated_drop_shadow.color.blue(), 255);
    EXPECT_EQ(interpolated_drop_shadow.color.alpha(), 128);

    VisualAnimationFilterOperation grayscale {
        .kind = VisualAnimationFilterOperationKind::Color,
        .amount = 0.75,
        .color_operation = Gfx::ColorFilterType::Grayscale,
    };
    EXPECT_EQ(grayscale.initial_value().amount, 0);
    EXPECT_EQ(grayscale.initial_value().interpolated_with(grayscale, 2).amount, 1);
}

TEST_CASE(rejects_non_finite_interpolated_filter_values)
{
    Vector<VisualAnimationFilterOperation> operations {
        { .kind = VisualAnimationFilterOperationKind::Blur, .amount = NumericLimits<float>::max() },
        { .kind = VisualAnimationFilterOperationKind::DropShadow, .amount = NumericLimits<float>::max(), .offset_x = NumericLimits<float>::max(), .offset_y = NumericLimits<float>::max() },
        { .kind = VisualAnimationFilterOperationKind::Color, .amount = NumericLimits<float>::max(), .color_operation = Gfx::ColorFilterType::Brightness },
        { .kind = VisualAnimationFilterOperationKind::HueRotate, .amount = NumericLimits<float>::max() },
    };
    for (auto const& operation : operations) {
        auto initial = operation;
        initial.amount = 0;
        initial.offset_x = 0;
        initial.offset_y = 0;
        VisualAnimation animation {
            .target_kind = VisualAnimation::TargetKind::Filter,
            .visual_context_node_indices = { 0 },
            .local_time_at_anchor_ms = 750,
            .iteration_duration_ms = 1000,
            .easing = linear_easing(),
            .keyframes = {
                { 0, { .kind = VisualAnimationEasing::Kind::Linear, .linear_points = { { 0, 0 }, { 1, 2 } } }, VisualAnimationFilterList { initial } },
                { 1, linear_easing(), VisualAnimationFilterList { operation } },
            },
        };

        EXPECT(animation.is_valid());
        EXPECT(!animation.sample(AK::Duration::zero()).has_value());
    }
}

TEST_CASE(rejects_invalid_timing)
{
    VisualAnimation animation;
    EXPECT(!animation.sample(AK::Duration::zero()).has_value());
}

TEST_CASE(applies_backwards_fill_during_start_delay)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 50,
        .start_delay_ms = 100,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.25f },
            { 1, linear_easing(), 0.75f },
        },
    };

    EXPECT(!animation.sample(AK::Duration::zero()).has_value());
    animation.fill_mode = VisualAnimationFillMode::Backwards;
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::zero())->opacity, 0.25f);
    EXPECT_APPROXIMATE(animation.sample(AK::Duration::from_milliseconds(550))->opacity, 0.5f);
}

TEST_CASE(applies_before_flag_only_to_effect_easing_during_start_delay)
{
    VisualAnimationEasing jump_start {
        .kind = VisualAnimationEasing::Kind::Steps,
        .linear_points = {},
        .interval_count = 4,
        .step_position = 0,
    };
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .local_time_at_anchor_ms = 50,
        .start_delay_ms = 100,
        .iteration_duration_ms = 1000,
        .fill_mode = VisualAnimationFillMode::Backwards,
        .easing = jump_start,
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };

    EXPECT_EQ(animation.sample(AK::Duration::zero())->opacity, 0.0f);

    animation.easing = linear_easing();
    animation.keyframes[0].easing = jump_start;
    EXPECT_EQ(animation.sample(AK::Duration::zero())->opacity, 0.25f);
}

TEST_CASE(compares_animation_parameters_independently_of_visual_nodes_and_anchor)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 1 },
        .monotonic_time_at_anchor_ns = 100,
        .local_time_at_anchor_ms = 250,
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };
    auto retargeted_animation = animation;
    retargeted_animation.visual_context_node_indices = { 2 };
    retargeted_animation.monotonic_time_at_anchor_ns = 200;
    retargeted_animation.local_time_at_anchor_ms = 500;

    EXPECT(animation.has_same_animation_parameters(retargeted_animation));
    EXPECT(!animation.has_same_parameters_except_anchor(retargeted_animation));

    retargeted_animation.iteration_duration_ms = 2000;
    EXPECT(!animation.has_same_animation_parameters(retargeted_animation));
}
