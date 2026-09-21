/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Compositor/VisualAnimation.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Compositor {

static_assert(to_underlying(VisualAnimationEasing::Kind::Linear) == to_underlying(Layout::RustFFI::FfiEasingKind::Linear));
static_assert(to_underlying(VisualAnimationEasing::Kind::CubicBezier) == to_underlying(Layout::RustFFI::FfiEasingKind::CubicBezier));
static_assert(to_underlying(VisualAnimationEasing::Kind::Steps) == to_underlying(Layout::RustFFI::FfiEasingKind::Steps));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Normal) == to_underlying(Animations::PlaybackDirection::Normal));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Reverse) == to_underlying(Animations::PlaybackDirection::Reverse));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Alternate) == to_underlying(Animations::PlaybackDirection::Alternate));
static_assert(to_underlying(VisualAnimationPlaybackDirection::AlternateReverse) == to_underlying(Animations::PlaybackDirection::AlternateReverse));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Normal) == to_underlying(Layout::RustFFI::FfiVisualAnimationPlaybackDirection::Normal));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Reverse) == to_underlying(Layout::RustFFI::FfiVisualAnimationPlaybackDirection::Reverse));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Alternate) == to_underlying(Layout::RustFFI::FfiVisualAnimationPlaybackDirection::Alternate));
static_assert(to_underlying(VisualAnimationPlaybackDirection::AlternateReverse) == to_underlying(Layout::RustFFI::FfiVisualAnimationPlaybackDirection::AlternateReverse));
static_assert(to_underlying(VisualAnimationFillMode::None) == to_underlying(Layout::RustFFI::FfiVisualAnimationFillMode::None));
static_assert(to_underlying(VisualAnimationFillMode::Backwards) == to_underlying(Layout::RustFFI::FfiVisualAnimationFillMode::Backwards));
static_assert(to_underlying(VisualAnimation::TargetKind::Opacity) == to_underlying(Layout::RustFFI::FfiVisualAnimationTargetKind::Opacity));
static_assert(to_underlying(VisualAnimation::TargetKind::BackgroundColor) == to_underlying(Layout::RustFFI::FfiVisualAnimationTargetKind::BackgroundColor));
static_assert(to_underlying(VisualAnimation::TargetKind::Filter) == to_underlying(Layout::RustFFI::FfiVisualAnimationTargetKind::Filter));
static_assert(to_underlying(VisualAnimation::TargetKind::Transform) == to_underlying(Layout::RustFFI::FfiVisualAnimationTargetKind::Transform));
static_assert(to_underlying(VisualAnimationFilterOperationKind::Blur) == to_underlying(Layout::RustFFI::FfiFilterFunctionKind::Blur));
static_assert(to_underlying(VisualAnimationFilterOperationKind::DropShadow) == to_underlying(Layout::RustFFI::FfiFilterFunctionKind::DropShadow));
static_assert(to_underlying(VisualAnimationFilterOperationKind::Color) == to_underlying(Layout::RustFFI::FfiFilterFunctionKind::Color));
static_assert(to_underlying(VisualAnimationFilterOperationKind::HueRotate) == to_underlying(Layout::RustFFI::FfiFilterFunctionKind::HueRotate));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Translate) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Translate));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Translate3d) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Translate3d));
static_assert(to_underlying(VisualAnimationTransformOperationKind::TranslateX) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::TranslateX));
static_assert(to_underlying(VisualAnimationTransformOperationKind::TranslateY) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::TranslateY));
static_assert(to_underlying(VisualAnimationTransformOperationKind::TranslateZ) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::TranslateZ));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Scale) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Scale));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Scale3d) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Scale3d));
static_assert(to_underlying(VisualAnimationTransformOperationKind::ScaleX) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::ScaleX));
static_assert(to_underlying(VisualAnimationTransformOperationKind::ScaleY) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::ScaleY));
static_assert(to_underlying(VisualAnimationTransformOperationKind::ScaleZ) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::ScaleZ));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Rotate) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Rotate));
static_assert(to_underlying(VisualAnimationTransformOperationKind::RotateX) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::RotateX));
static_assert(to_underlying(VisualAnimationTransformOperationKind::RotateY) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::RotateY));
static_assert(to_underlying(VisualAnimationTransformOperationKind::RotateZ) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::RotateZ));
static_assert(to_underlying(VisualAnimationTransformOperationKind::Skew) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::Skew));
static_assert(to_underlying(VisualAnimationTransformOperationKind::SkewX) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::SkewX));
static_assert(to_underlying(VisualAnimationTransformOperationKind::SkewY) == to_underlying(Layout::RustFFI::FfiVisualAnimationTransformOperationKind::SkewY));

VisualAnimationEasing VisualAnimationEasing::from_css(CSS::EasingFunction const& easing)
{
    return easing.visit(
        [](CSS::LinearEasingFunction const& linear) {
            VisualAnimationEasing result;
            result.kind = Kind::Linear;
            result.linear_points.clear();
            result.linear_points.ensure_capacity(linear.control_points.size());
            for (auto const& point : linear.control_points)
                result.linear_points.unchecked_append({ point.input, point.output });
            return result;
        },
        [](CSS::CubicBezierEasingFunction const& cubic_bezier) {
            return VisualAnimationEasing {
                .kind = Kind::CubicBezier,
                .linear_points = {},
                .x1 = cubic_bezier.x1,
                .y1 = cubic_bezier.y1,
                .x2 = cubic_bezier.x2,
                .y2 = cubic_bezier.y2,
            };
        },
        [](CSS::StepsEasingFunction const& steps) {
            return VisualAnimationEasing {
                .kind = Kind::Steps,
                .linear_points = {},
                .interval_count = steps.interval_count,
                .step_position = to_underlying(steps.position),
            };
        });
}

bool VisualAnimationTransformOperation::is_valid() const
{
    return Layout::RustFFI::visual_animation_transform_operation_is_valid(
        static_cast<Layout::RustFFI::FfiVisualAnimationTransformOperationKind>(to_underlying(kind)), values.data(), values.size());
}

bool VisualAnimation::has_same_parameters_except_anchor(VisualAnimation const& other) const
{
    return target_kind == other.target_kind
        && visual_context_node_indices == other.visual_context_node_indices
        && has_same_animation_parameters(other);
}

bool VisualAnimation::has_same_animation_parameters(VisualAnimation const& other) const
{
    return target_kind == other.target_kind
        && playback_rate == other.playback_rate
        && start_delay_ms == other.start_delay_ms
        && iteration_duration_ms == other.iteration_duration_ms
        && iteration_count == other.iteration_count
        && iteration_start == other.iteration_start
        && playback_direction == other.playback_direction
        && fill_mode == other.fill_mode
        && easing == other.easing
        && keyframes == other.keyframes;
}

bool VisualAnimation::is_valid() const
{
    return !visual_context_node_indices.is_empty() && has_valid_animation_parameters();
}

bool VisualAnimation::has_valid_animation_parameters() const
{
    Painting::VisualAnimationFfiDescriptors descriptors { ReadonlySpan<VisualAnimation> { this, 1 } };
    return Layout::RustFFI::visual_animation_parameters_are_valid(descriptors.descriptors().data());
}

}
