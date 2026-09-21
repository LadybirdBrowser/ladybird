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

TEST_CASE(validates_timing_keyframes_and_transform_operations)
{
    VisualAnimation animation {
        .target_kind = VisualAnimation::TargetKind::Opacity,
        .visual_context_node_indices = { 0 },
        .iteration_duration_ms = 1000,
        .easing = linear_easing(),
        .keyframes = {
            { 0, linear_easing(), 0.0f },
            { 1, linear_easing(), 1.0f },
        },
    };
    EXPECT(animation.is_valid());
    EXPECT(animation.has_valid_animation_parameters());

    auto without_target_nodes = animation;
    without_target_nodes.visual_context_node_indices.clear();
    EXPECT(!without_target_nodes.is_valid());
    EXPECT(without_target_nodes.has_valid_animation_parameters());

    auto without_duration = animation;
    without_duration.iteration_duration_ms = 0;
    EXPECT(!without_duration.is_valid());

    auto with_unordered_keyframes = animation;
    with_unordered_keyframes.keyframes[0].offset = 1;
    with_unordered_keyframes.keyframes[1].offset = 0;
    EXPECT(!with_unordered_keyframes.is_valid());

    auto with_overshooting_opacity = animation;
    with_overshooting_opacity.keyframes[1].value = 1.5f;
    EXPECT(!with_overshooting_opacity.is_valid());

    auto with_a_steps_easing_without_intervals = animation;
    with_a_steps_easing_without_intervals.easing = VisualAnimationEasing { .kind = VisualAnimationEasing::Kind::Steps, .linear_points = {}, .interval_count = 0 };
    EXPECT(!with_a_steps_easing_without_intervals.is_valid());

    EXPECT(!VisualAnimation {}.is_valid());

    VisualAnimationTransformOperation translate { VisualAnimationTransformOperationKind::Translate, { 1, 2 } };
    EXPECT(translate.is_valid());
    VisualAnimationTransformOperation translate_with_three_values { VisualAnimationTransformOperationKind::Translate, { 1, 2, 3 } };
    EXPECT(!translate_with_three_values.is_valid());
    VisualAnimationTransformOperation rotate_by_infinity { VisualAnimationTransformOperationKind::Rotate, { AK::Infinity<float> } };
    EXPECT(!rotate_by_infinity.is_valid());
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
