/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Compositor/VisualAnimation.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Compositor {

static_assert(to_underlying(VisualAnimationEasing::Kind::Linear) == to_underlying(CSS::StyleValueFFI::FfiEasingKind::Linear));
static_assert(to_underlying(VisualAnimationEasing::Kind::CubicBezier) == to_underlying(CSS::StyleValueFFI::FfiEasingKind::CubicBezier));
static_assert(to_underlying(VisualAnimationEasing::Kind::Steps) == to_underlying(CSS::StyleValueFFI::FfiEasingKind::Steps));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Normal) == to_underlying(Animations::PlaybackDirection::Normal));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Reverse) == to_underlying(Animations::PlaybackDirection::Reverse));
static_assert(to_underlying(VisualAnimationPlaybackDirection::Alternate) == to_underlying(Animations::PlaybackDirection::Alternate));
static_assert(to_underlying(VisualAnimationPlaybackDirection::AlternateReverse) == to_underlying(Animations::PlaybackDirection::AlternateReverse));

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

double VisualAnimationEasing::evaluate_at(double input_progress, bool before_flag) const
{
    Vector<CSS::StyleValueFFI::FfiLinearEasingPoint> points;
    if (kind == Kind::Linear) {
        points.ensure_capacity(linear_points.size());
        for (auto const& point : linear_points)
            points.unchecked_append({ .input = point.input, .output = point.output });
    }

    CSS::StyleValueFFI::FfiEasingDescriptor descriptor {
        .kind = static_cast<CSS::StyleValueFFI::FfiEasingKind>(to_underlying(kind)),
        .linear_points = points.data(),
        .linear_point_count = points.size(),
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2,
        .interval_count = interval_count,
        .step_position = step_position,
    };
    return CSS::StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

bool VisualAnimationEasing::is_valid() const
{
    if (!first_is_one_of(kind, Kind::Linear, Kind::CubicBezier, Kind::Steps))
        return false;
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
        return false;

    switch (kind) {
    case Kind::Linear: {
        if (linear_points.size() < 2)
            return false;
        double previous_input = -NumericLimits<double>::max();
        for (auto const& point : linear_points) {
            if (!isfinite(point.input) || !isfinite(point.output) || point.input < previous_input)
                return false;
            previous_input = point.input;
        }
        return true;
    }
    case Kind::CubicBezier:
        return x1 >= 0 && x1 <= 1 && x2 >= 0 && x2 <= 1;
    case Kind::Steps:
        return interval_count > 0 && step_position <= 5;
    }
    VERIFY_NOT_REACHED();
}

Gfx::FloatMatrix4x4 VisualAnimationTransformOperation::to_matrix() const
{
    using Kind = VisualAnimationTransformOperationKind;
    switch (kind) {
    case Kind::Translate:
        VERIFY(values.size() == 1 || values.size() == 2);
        return Gfx::translation_matrix(Gfx::FloatVector3 { values[0], values.size() == 2 ? values[1] : 0, 0 });
    case Kind::Translate3d:
        VERIFY(values.size() == 3);
        return Gfx::translation_matrix(Gfx::FloatVector3 { values[0], values[1], values[2] });
    case Kind::TranslateX:
        VERIFY(values.size() == 1);
        return Gfx::translation_matrix(Gfx::FloatVector3 { values[0], 0, 0 });
    case Kind::TranslateY:
        VERIFY(values.size() == 1);
        return Gfx::translation_matrix(Gfx::FloatVector3 { 0, values[0], 0 });
    case Kind::TranslateZ:
        VERIFY(values.size() == 1);
        return Gfx::translation_matrix(Gfx::FloatVector3 { 0, 0, values[0] });
    case Kind::Scale: {
        VERIFY(values.size() == 1 || values.size() == 2);
        auto y = values.size() == 2 ? values[1] : values[0];
        return Gfx::scale_matrix(Gfx::FloatVector3 { values[0], y, 1 });
    }
    case Kind::Scale3d:
        VERIFY(values.size() == 3);
        return Gfx::scale_matrix(Gfx::FloatVector3 { values[0], values[1], values[2] });
    case Kind::ScaleX:
        VERIFY(values.size() == 1);
        return Gfx::scale_matrix(Gfx::FloatVector3 { values[0], 1, 1 });
    case Kind::ScaleY:
        VERIFY(values.size() == 1);
        return Gfx::scale_matrix(Gfx::FloatVector3 { 1, values[0], 1 });
    case Kind::ScaleZ:
        VERIFY(values.size() == 1);
        return Gfx::scale_matrix(Gfx::FloatVector3 { 1, 1, values[0] });
    case Kind::Rotate:
    case Kind::RotateZ:
        VERIFY(values.size() == 1);
        return Gfx::rotation_matrix(Gfx::FloatVector3 { 0, 0, 1 }, values[0]);
    case Kind::RotateX:
        VERIFY(values.size() == 1);
        return Gfx::rotation_matrix(Gfx::FloatVector3 { 1, 0, 0 }, values[0]);
    case Kind::RotateY:
        VERIFY(values.size() == 1);
        return Gfx::rotation_matrix(Gfx::FloatVector3 { 0, 1, 0 }, values[0]);
    case Kind::Skew:
        VERIFY(values.size() == 1 || values.size() == 2);
        return Gfx::FloatMatrix4x4(
            1, tanf(values[0]), 0, 0,
            values.size() == 2 ? tanf(values[1]) : 0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
    case Kind::SkewX:
        VERIFY(values.size() == 1);
        return Gfx::FloatMatrix4x4(
            1, tanf(values[0]), 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
    case Kind::SkewY:
        VERIFY(values.size() == 1);
        return Gfx::FloatMatrix4x4(
            1, 0, 0, 0,
            tanf(values[0]), 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
    }
    VERIFY_NOT_REACHED();
}

bool VisualAnimationTransformOperation::is_valid() const
{
    size_t expected_value_count = 0;
    switch (kind) {
    case VisualAnimationTransformOperationKind::Translate:
    case VisualAnimationTransformOperationKind::Scale:
    case VisualAnimationTransformOperationKind::Skew:
        if (values.size() != 1 && values.size() != 2)
            return false;
        expected_value_count = values.size();
        break;
    case VisualAnimationTransformOperationKind::Translate3d:
    case VisualAnimationTransformOperationKind::Scale3d:
        expected_value_count = 3;
        break;
    case VisualAnimationTransformOperationKind::TranslateX:
    case VisualAnimationTransformOperationKind::TranslateY:
    case VisualAnimationTransformOperationKind::TranslateZ:
    case VisualAnimationTransformOperationKind::ScaleX:
    case VisualAnimationTransformOperationKind::ScaleY:
    case VisualAnimationTransformOperationKind::ScaleZ:
    case VisualAnimationTransformOperationKind::Rotate:
    case VisualAnimationTransformOperationKind::RotateX:
    case VisualAnimationTransformOperationKind::RotateY:
    case VisualAnimationTransformOperationKind::RotateZ:
    case VisualAnimationTransformOperationKind::SkewX:
    case VisualAnimationTransformOperationKind::SkewY:
        expected_value_count = 1;
        break;
    default:
        return false;
    }
    if (values.size() != expected_value_count)
        return false;
    return all_of(values, [](float value) { return isfinite(value); });
}

static Gfx::Color interpolate_legacy_srgb_color(Gfx::Color from, Gfx::Color to, double progress)
{
    auto from_alpha = static_cast<double>(from.alpha()) / 255.0;
    auto to_alpha = static_cast<double>(to.alpha()) / 255.0;
    auto alpha = clamp(from_alpha + (to_alpha - from_alpha) * progress, 0.0, 1.0);
    if (alpha == 0)
        return Gfx::Color::Transparent;
    auto interpolate_channel = [&](u8 from_channel, u8 to_channel) {
        auto from_premultiplied = static_cast<double>(from_channel) * from_alpha;
        auto to_premultiplied = static_cast<double>(to_channel) * to_alpha;
        auto channel = (from_premultiplied + (to_premultiplied - from_premultiplied) * progress) / alpha;
        return round_to<u8>(clamp(channel, 0.0, 255.0));
    };
    return Gfx::Color {
        interpolate_channel(from.red(), to.red()),
        interpolate_channel(from.green(), to.green()),
        interpolate_channel(from.blue(), to.blue()),
        round_to<u8>(alpha * 255.0),
    };
}

bool VisualAnimationFilterOperation::matches(VisualAnimationFilterOperation const& other) const
{
    return kind == other.kind
        && (kind != VisualAnimationFilterOperationKind::Color || color_operation == other.color_operation);
}

VisualAnimationFilterOperation VisualAnimationFilterOperation::initial_value() const
{
    auto initial = *this;
    initial.amount = 0;
    initial.offset_x = 0;
    initial.offset_y = 0;
    initial.color = Gfx::Color::Transparent;
    if (kind == VisualAnimationFilterOperationKind::Color
        && !first_is_one_of(color_operation, Gfx::ColorFilterType::Grayscale, Gfx::ColorFilterType::Invert, Gfx::ColorFilterType::Sepia))
        initial.amount = 1;
    return initial;
}

VisualAnimationFilterOperation VisualAnimationFilterOperation::interpolated_with(VisualAnimationFilterOperation const& other, double progress) const
{
    VERIFY(matches(other));
    auto interpolate = [&](float from, float to) {
        return static_cast<float>(from + (to - from) * progress);
    };
    auto result = *this;
    result.amount = interpolate(amount, other.amount);
    result.offset_x = interpolate(offset_x, other.offset_x);
    result.offset_y = interpolate(offset_y, other.offset_y);
    if (kind == VisualAnimationFilterOperationKind::DropShadow)
        result.color = interpolate_legacy_srgb_color(color, other.color, progress);

    if (first_is_one_of(kind, VisualAnimationFilterOperationKind::Blur, VisualAnimationFilterOperationKind::DropShadow))
        result.amount = max(result.amount, 0.0f);
    if (kind == VisualAnimationFilterOperationKind::Color) {
        result.amount = max(result.amount, 0.0f);
        if (first_is_one_of(color_operation, Gfx::ColorFilterType::Grayscale, Gfx::ColorFilterType::Invert, Gfx::ColorFilterType::Opacity, Gfx::ColorFilterType::Sepia))
            result.amount = min(result.amount, 1.0f);
    }
    return result;
}

bool VisualAnimationFilterOperation::is_valid() const
{
    if (!first_is_one_of(kind,
            VisualAnimationFilterOperationKind::Blur,
            VisualAnimationFilterOperationKind::DropShadow,
            VisualAnimationFilterOperationKind::Color,
            VisualAnimationFilterOperationKind::HueRotate))
        return false;
    if (!isfinite(amount) || !isfinite(offset_x) || !isfinite(offset_y))
        return false;
    if (first_is_one_of(kind, VisualAnimationFilterOperationKind::Blur, VisualAnimationFilterOperationKind::DropShadow) && amount < 0)
        return false;
    if (kind != VisualAnimationFilterOperationKind::Color)
        return true;
    if (!first_is_one_of(color_operation,
            Gfx::ColorFilterType::Brightness,
            Gfx::ColorFilterType::Contrast,
            Gfx::ColorFilterType::Grayscale,
            Gfx::ColorFilterType::Invert,
            Gfx::ColorFilterType::Opacity,
            Gfx::ColorFilterType::Saturate,
            Gfx::ColorFilterType::Sepia))
        return false;
    if (amount < 0)
        return false;
    return !first_is_one_of(color_operation, Gfx::ColorFilterType::Grayscale, Gfx::ColorFilterType::Invert, Gfx::ColorFilterType::Opacity, Gfx::ColorFilterType::Sepia)
        || amount <= 1;
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

static VisualAnimationValue interpolate_value(VisualAnimationValue const& from, VisualAnimationValue const& to, double progress)
{
    VERIFY(from.index() == to.index());
    return from.visit(
        [&](float from_opacity) -> VisualAnimationValue {
            auto to_opacity = to.get<float>();
            return static_cast<float>(from_opacity + (to_opacity - from_opacity) * progress);
        },
        [&](Gfx::Color from_color) -> VisualAnimationValue {
            return interpolate_legacy_srgb_color(from_color, to.get<Gfx::Color>(), progress);
        },
        [&](VisualAnimationFilterList const& from_filters) -> VisualAnimationValue {
            auto padded_from_filters = from_filters;
            auto padded_to_filters = to.get<VisualAnimationFilterList>();
            while (padded_from_filters.size() < padded_to_filters.size())
                padded_from_filters.append(padded_to_filters[padded_from_filters.size()].initial_value());
            while (padded_to_filters.size() < padded_from_filters.size())
                padded_to_filters.append(padded_from_filters[padded_to_filters.size()].initial_value());
            for (size_t index = 0; index < padded_from_filters.size(); ++index) {
                if (!padded_from_filters[index].matches(padded_to_filters[index]))
                    return progress < 0.5 ? from_filters : to.get<VisualAnimationFilterList>();
            }
            VisualAnimationFilterList filters;
            filters.ensure_capacity(padded_from_filters.size());
            for (size_t index = 0; index < padded_from_filters.size(); ++index)
                filters.unchecked_append(padded_from_filters[index].interpolated_with(padded_to_filters[index], progress));
            return filters;
        },
        [&](VisualAnimationTransformList const& from_transforms) -> VisualAnimationValue {
            auto const& to_transforms = to.get<VisualAnimationTransformList>();
            VERIFY(from_transforms.size() == to_transforms.size());
            VisualAnimationTransformList transforms;
            transforms.ensure_capacity(from_transforms.size());
            for (size_t operation_index = 0; operation_index < from_transforms.size(); ++operation_index) {
                auto const& from_operation = from_transforms[operation_index];
                auto const& to_operation = to_transforms[operation_index];
                VERIFY(from_operation.kind == to_operation.kind);
                VERIFY(from_operation.values.size() == to_operation.values.size());
                Vector<float> values;
                values.ensure_capacity(from_operation.values.size());
                for (size_t value_index = 0; value_index < from_operation.values.size(); ++value_index) {
                    auto from_value = from_operation.values[value_index];
                    auto to_value = to_operation.values[value_index];
                    values.unchecked_append(static_cast<float>(from_value + (to_value - from_value) * progress));
                }
                transforms.unchecked_append({ from_operation.kind, move(values) });
            }
            return transforms;
        });
}

Optional<VisualAnimation::Sample> VisualAnimation::sample(AK::Duration elapsed_since_anchor) const
{
    if (iteration_duration_ms <= 0 || playback_rate <= 0 || keyframes.size() < 2)
        return {};

    auto local_time = local_time_at_anchor_ms + elapsed_since_anchor.to_seconds_f64() * 1000.0 * playback_rate;
    if (!isfinite(local_time))
        return {};
    bool is_in_before_phase = local_time < start_delay_ms;
    if (is_in_before_phase) {
        if (fill_mode != VisualAnimationFillMode::Backwards)
            return {};
        local_time = start_delay_ms;
    }
    auto overall_progress = (local_time - start_delay_ms) / iteration_duration_ms + iteration_start;
    if (!isfinite(overall_progress) || overall_progress < 0)
        return {};

    auto active_progress = overall_progress - iteration_start;
    bool is_at_or_after_end = isfinite(iteration_count) && active_progress >= iteration_count;
    auto current_iteration = floor(overall_progress);
    auto simple_iteration_progress = fmod(overall_progress, 1.0);
    if (is_at_or_after_end) {
        auto terminal_overall_progress = iteration_start + iteration_count;
        simple_iteration_progress = fmod(terminal_overall_progress, 1.0);
        if (simple_iteration_progress == 0)
            simple_iteration_progress = 1.0;
        current_iteration = floor(terminal_overall_progress) - (simple_iteration_progress == 1.0 ? 1.0 : 0.0);
    }
    bool going_forwards = true;
    switch (playback_direction) {
    case VisualAnimationPlaybackDirection::Normal:
        break;
    case VisualAnimationPlaybackDirection::Reverse:
        going_forwards = false;
        break;
    case VisualAnimationPlaybackDirection::Alternate:
        going_forwards = fmod(current_iteration, 2.0) == 0;
        break;
    case VisualAnimationPlaybackDirection::AlternateReverse:
        going_forwards = fmod(current_iteration + 1.0, 2.0) == 0;
        break;
    }

    auto directed_progress = going_forwards ? simple_iteration_progress : 1.0 - simple_iteration_progress;
    bool before_flag = (is_in_before_phase && going_forwards) || (is_at_or_after_end && !going_forwards);
    auto transformed_progress = easing.evaluate_at(directed_progress, before_flag);
    if (!isfinite(transformed_progress))
        return {};

    auto const* from_keyframe = &keyframes.first();
    auto const* to_keyframe = &keyframes[1];
    for (size_t index = 1; index < keyframes.size(); ++index) {
        if (transformed_progress < keyframes[index].offset) {
            from_keyframe = &keyframes[index - 1];
            to_keyframe = &keyframes[index];
            break;
        }
        from_keyframe = &keyframes[index - 1];
        to_keyframe = &keyframes[index];
    }

    auto interval_width = to_keyframe->offset - from_keyframe->offset;
    VERIFY(interval_width > 0);
    auto interval_progress = (transformed_progress - from_keyframe->offset) / interval_width;
    auto eased_interval_progress = from_keyframe->easing.evaluate_at(interval_progress, false);
    if (!isfinite(eased_interval_progress))
        return {};
    auto value = interpolate_value(from_keyframe->value, to_keyframe->value, eased_interval_progress);
    if (value.has<VisualAnimationFilterList>()
        && any_of(value.get<VisualAnimationFilterList>(), [](auto const& operation) { return !operation.is_valid(); }))
        return {};

    Sample sample;
    value.visit(
        [&](float opacity) { sample.opacity = opacity; },
        [&](Gfx::Color background_color) { sample.background_color = background_color; },
        [&](VisualAnimationFilterList const& filters) {
            sample.samples_filter = true;
            Vector<Layout::RustFFI::FfiFilterFunction> functions;
            functions.ensure_capacity(filters.size());
            for (auto const& operation : filters) {
                Layout::RustFFI::FfiFilterFunction function {};
                function.kind = [&] {
                    switch (operation.kind) {
                    case VisualAnimationFilterOperationKind::Blur:
                        return Layout::RustFFI::FfiFilterFunctionKind::Blur;
                    case VisualAnimationFilterOperationKind::DropShadow:
                        return Layout::RustFFI::FfiFilterFunctionKind::DropShadow;
                    case VisualAnimationFilterOperationKind::Color:
                        return Layout::RustFFI::FfiFilterFunctionKind::Color;
                    case VisualAnimationFilterOperationKind::HueRotate:
                        return Layout::RustFFI::FfiFilterFunctionKind::HueRotate;
                    }
                    VERIFY_NOT_REACHED();
                }();
                function.amount = operation.amount;
                function.offset_x = operation.offset_x;
                function.offset_y = operation.offset_y;
                function.color = operation.color;
                function.color_operation = operation.color_operation;
                functions.unchecked_append(function);
            }
            if (auto filter = Painting::filter_from_functions(functions); filter.has_value())
                sample.filter_bytes = move(*filter).take_serialized_bytes();
        },
        [&](VisualAnimationTransformList const& transforms) {
            for (auto const& transform : transforms)
                sample.transform = sample.transform * transform.to_matrix();
        });
    if (target_kind == TargetKind::Opacity) {
        if (!isfinite(sample.opacity))
            return {};
        sample.opacity = clamp(sample.opacity, 0.0f, 1.0f);
        return sample;
    }
    if (target_kind == TargetKind::BackgroundColor) {
        if (!sample.background_color.has_value())
            return {};
        return sample;
    }
    if (target_kind == TargetKind::Filter) {
        if (!sample.samples_filter)
            return {};
        return sample;
    }
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            if (!isfinite(sample.transform[row, column]))
                return {};
        }
    }
    return sample;
}

bool VisualAnimation::is_valid() const
{
    if (!first_is_one_of(target_kind, TargetKind::Opacity, TargetKind::BackgroundColor, TargetKind::Filter, TargetKind::Transform))
        return false;
    if (visual_context_node_indices.is_empty())
        return false;
    if (!first_is_one_of(playback_direction,
            VisualAnimationPlaybackDirection::Normal,
            VisualAnimationPlaybackDirection::Reverse,
            VisualAnimationPlaybackDirection::Alternate,
            VisualAnimationPlaybackDirection::AlternateReverse))
        return false;
    if (!first_is_one_of(fill_mode, VisualAnimationFillMode::None, VisualAnimationFillMode::Backwards))
        return false;
    if (monotonic_time_at_anchor_ns < 0 || !isfinite(local_time_at_anchor_ms) || !isfinite(playback_rate) || playback_rate <= 0
        || !isfinite(start_delay_ms) || !isfinite(iteration_duration_ms) || iteration_duration_ms <= 0
        || isnan(iteration_count) || iteration_count <= 0
        || !isfinite(iteration_start) || iteration_start < 0 || !easing.is_valid() || keyframes.size() < 2)
        return false;
    if (keyframes.first().offset != 0 || keyframes.last().offset != 1)
        return false;

    Optional<size_t> transform_operation_count;
    for (size_t keyframe_index = 0; keyframe_index < keyframes.size(); ++keyframe_index) {
        auto const& keyframe = keyframes[keyframe_index];
        if (!isfinite(keyframe.offset) || keyframe.offset < 0 || keyframe.offset > 1 || !keyframe.easing.is_valid())
            return false;
        if (keyframe_index > 0 && keyframe.offset <= keyframes[keyframe_index - 1].offset)
            return false;

        if (target_kind == TargetKind::Opacity) {
            if (!keyframe.value.has<float>() || !isfinite(keyframe.value.get<float>())
                || keyframe.value.get<float>() < 0 || keyframe.value.get<float>() > 1)
                return false;
            continue;
        }

        if (target_kind == TargetKind::BackgroundColor) {
            if (!keyframe.value.has<Gfx::Color>())
                return false;
            continue;
        }

        if (target_kind == TargetKind::Filter) {
            if (!keyframe.value.has<VisualAnimationFilterList>())
                return false;
            if (any_of(keyframe.value.get<VisualAnimationFilterList>(), [](auto const& operation) { return !operation.is_valid(); }))
                return false;
            continue;
        }

        if (!keyframe.value.has<VisualAnimationTransformList>())
            return false;
        auto const& transforms = keyframe.value.get<VisualAnimationTransformList>();
        if (transforms.is_empty())
            return false;
        if (!transform_operation_count.has_value())
            transform_operation_count = transforms.size();
        if (transforms.size() != *transform_operation_count)
            return false;
        for (size_t operation_index = 0; operation_index < transforms.size(); ++operation_index) {
            auto const& operation = transforms[operation_index];
            if (!operation.is_valid())
                return false;
            if (keyframe_index > 0) {
                auto const& previous_operation = keyframes[keyframe_index - 1].value.get<VisualAnimationTransformList>()[operation_index];
                if (operation.kind != previous_operation.kind || operation.values.size() != previous_operation.values.size())
                    return false;
            }
        }
    }
    return true;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimationEasing::LinearPoint const& point)
{
    TRY(encoder.encode(point.input));
    TRY(encoder.encode(point.output));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimationEasing::LinearPoint> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimationEasing::LinearPoint {
        .input = TRY(decoder.decode<double>()),
        .output = TRY(decoder.decode<double>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimationEasing const& easing)
{
    TRY(encoder.encode(easing.kind));
    TRY(encoder.encode(easing.linear_points));
    TRY(encoder.encode(easing.x1));
    TRY(encoder.encode(easing.y1));
    TRY(encoder.encode(easing.x2));
    TRY(encoder.encode(easing.y2));
    TRY(encoder.encode(easing.interval_count));
    TRY(encoder.encode(easing.step_position));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimationEasing> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimationEasing {
        .kind = TRY(decoder.decode<Web::Compositor::VisualAnimationEasing::Kind>()),
        .linear_points = TRY(decoder.decode<Vector<Web::Compositor::VisualAnimationEasing::LinearPoint>>()),
        .x1 = TRY(decoder.decode<double>()),
        .y1 = TRY(decoder.decode<double>()),
        .x2 = TRY(decoder.decode<double>()),
        .y2 = TRY(decoder.decode<double>()),
        .interval_count = TRY(decoder.decode<i32>()),
        .step_position = TRY(decoder.decode<u8>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimationTransformOperation const& operation)
{
    TRY(encoder.encode(operation.kind));
    TRY(encoder.encode(operation.values));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimationTransformOperation> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimationTransformOperation {
        .kind = TRY(decoder.decode<Web::Compositor::VisualAnimationTransformOperationKind>()),
        .values = TRY(decoder.decode<Vector<float>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimationFilterOperation const& operation)
{
    TRY(encoder.encode(operation.kind));
    TRY(encoder.encode(operation.amount));
    TRY(encoder.encode(operation.offset_x));
    TRY(encoder.encode(operation.offset_y));
    TRY(encoder.encode(operation.color));
    TRY(encoder.encode(operation.color_operation));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimationFilterOperation> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimationFilterOperation {
        .kind = TRY(decoder.decode<Web::Compositor::VisualAnimationFilterOperationKind>()),
        .amount = TRY(decoder.decode<float>()),
        .offset_x = TRY(decoder.decode<float>()),
        .offset_y = TRY(decoder.decode<float>()),
        .color = TRY(decoder.decode<Gfx::Color>()),
        .color_operation = TRY(decoder.decode<Gfx::ColorFilterType>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimationKeyframe const& keyframe)
{
    TRY(encoder.encode(keyframe.offset));
    TRY(encoder.encode(keyframe.easing));
    TRY(encoder.encode(keyframe.value));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimationKeyframe> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimationKeyframe {
        .offset = TRY(decoder.decode<double>()),
        .easing = TRY(decoder.decode<Web::Compositor::VisualAnimationEasing>()),
        .value = TRY(decoder.decode<Web::Compositor::VisualAnimationValue>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::VisualAnimation const& animation)
{
    TRY(encoder.encode(animation.target_kind));
    TRY(encoder.encode(animation.visual_context_node_indices));
    TRY(encoder.encode(animation.monotonic_time_at_anchor_ns));
    TRY(encoder.encode(animation.local_time_at_anchor_ms));
    TRY(encoder.encode(animation.playback_rate));
    TRY(encoder.encode(animation.start_delay_ms));
    TRY(encoder.encode(animation.iteration_duration_ms));
    TRY(encoder.encode(animation.iteration_count));
    TRY(encoder.encode(animation.iteration_start));
    TRY(encoder.encode(animation.playback_direction));
    TRY(encoder.encode(animation.fill_mode));
    TRY(encoder.encode(animation.easing));
    TRY(encoder.encode(animation.keyframes));
    return {};
}

template<>
ErrorOr<Web::Compositor::VisualAnimation> decode(Decoder& decoder)
{
    return Web::Compositor::VisualAnimation {
        .target_kind = TRY(decoder.decode<Web::Compositor::VisualAnimation::TargetKind>()),
        .visual_context_node_indices = TRY(decoder.decode<Vector<u32>>()),
        .monotonic_time_at_anchor_ns = TRY(decoder.decode<i64>()),
        .local_time_at_anchor_ms = TRY(decoder.decode<double>()),
        .playback_rate = TRY(decoder.decode<double>()),
        .start_delay_ms = TRY(decoder.decode<double>()),
        .iteration_duration_ms = TRY(decoder.decode<double>()),
        .iteration_count = TRY(decoder.decode<double>()),
        .iteration_start = TRY(decoder.decode<double>()),
        .playback_direction = TRY(decoder.decode<Web::Compositor::VisualAnimationPlaybackDirection>()),
        .fill_mode = TRY(decoder.decode<Web::Compositor::VisualAnimationFillMode>()),
        .easing = TRY(decoder.decode<Web::Compositor::VisualAnimationEasing>()),
        .keyframes = TRY(decoder.decode<Vector<Web::Compositor::VisualAnimationKeyframe>>()),
    };
}

}
