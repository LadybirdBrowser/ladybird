/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EasingFunction.h"
#include <AK/Math.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-easing/#linear-easing-function-output
double LinearEasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    Vector<StyleValueFFI::FfiLinearEasingPoint> points;
    points.ensure_capacity(control_points.size());
    for (auto const& point : control_points)
        points.unchecked_append({ .input = point.input, .output = point.output });
    StyleValueFFI::FfiEasingDescriptor descriptor {
        .kind = StyleValueFFI::FfiEasingKind::Linear,
        .linear_points = points.data(),
        .linear_point_count = points.size(),
        .x1 = 0,
        .y1 = 0,
        .x2 = 0,
        .y2 = 0,
        .interval_count = 0,
        .step_position = 0,
    };
    return StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

// https://www.w3.org/TR/css-easing-1/#cubic-bezier-algo
double CubicBezierEasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    StyleValueFFI::FfiEasingDescriptor descriptor {
        .kind = StyleValueFFI::FfiEasingKind::CubicBezier,
        .linear_points = nullptr,
        .linear_point_count = 0,
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2,
        .interval_count = 0,
        .step_position = 0,
    };
    return StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

// https://www.w3.org/TR/css-easing-1/#step-easing-algo
double StepsEasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    StyleValueFFI::FfiEasingDescriptor descriptor {
        .kind = StyleValueFFI::FfiEasingKind::Steps,
        .linear_points = nullptr,
        .linear_point_count = 0,
        .x1 = 0,
        .y1 = 0,
        .x2 = 0,
        .y2 = 0,
        .interval_count = interval_count,
        .step_position = to_underlying(position),
    };
    return StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

// https://drafts.csswg.org/css-easing/#linear-canonicalization
struct UnresolvedLinearEasingControlPoint {
    Optional<double> input;
    double output;
};

static Vector<LinearEasingFunction::ControlPoint> canonicalize_linear_easing_function_control_points(Vector<UnresolvedLinearEasingControlPoint> control_points)
{
    // To canonicalize a linear() function’s control points, perform the following:
    auto canonicalized_control_points = move(control_points);

    // 1. If the first control point lacks an input progress value, set its input progress value to 0.
    if (!canonicalized_control_points.first().input.has_value())
        canonicalized_control_points.first().input = 0;

    // 2. If the last control point lacks an input progress value, set its input progress value to 1.
    if (!canonicalized_control_points.last().input.has_value())
        canonicalized_control_points.last().input = 1;

    // 3. If any control point has an input progress value that is less than
    // the input progress value of any preceding control point,
    // set its input progress value to the largest input progress value of any preceding control point.
    double largest_input = 0;
    for (auto& control_point : canonicalized_control_points) {
        if (control_point.input.has_value()) {
            if (control_point.input.value() < largest_input) {
                control_point.input = largest_input;
            } else {
                largest_input = control_point.input.value();
            }
        }
    }

    // 4. If any control point still lacks an input progress value,
    // then for each contiguous run of such control points,
    // set their input progress values so that they are evenly spaced
    // between the preceding and following control points with input progress values.
    Optional<size_t> run_start_idx;
    for (size_t idx = 0; idx < canonicalized_control_points.size(); idx++) {
        auto& control_point = canonicalized_control_points[idx];
        if (control_point.input.has_value() && run_start_idx.has_value()) {
            // Note: this stop is immediately after a run
            //       set inputs of [start, idx-1] stops to be evenly spaced between start-1 and idx
            auto start_input = canonicalized_control_points[run_start_idx.value() - 1].input.value();
            auto end_input = canonicalized_control_points[idx].input.value();
            auto run_stop_count = idx - run_start_idx.value() + 1;
            auto delta = (end_input - start_input) / run_stop_count;
            for (size_t run_idx = 0; run_idx < run_stop_count; run_idx++) {
                canonicalized_control_points[run_idx + run_start_idx.value() - 1].input = start_input + delta * run_idx;
            }
            run_start_idx = {};
        } else if (!control_point.input.has_value() && !run_start_idx.has_value()) {
            // Note: this stop is the start of a run
            run_start_idx = idx;
        }
    }

    Vector<LinearEasingFunction::ControlPoint> resolved_control_points;
    resolved_control_points.ensure_capacity(canonicalized_control_points.size());
    for (auto const& control_point : canonicalized_control_points)
        resolved_control_points.unchecked_append({ control_point.input.value(), control_point.output });
    return resolved_control_points;
}

// https://drafts.csswg.org/css-easing-2/#linear-easing-function
EasingFunction EasingFunction::linear()
{
    // Equivalent to linear(0, 1)
    return LinearEasingFunction { { { 0, 0 }, { 1, 1 } }, "linear"_utf16 };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-in
EasingFunction EasingFunction::ease_in()
{
    // Equivalent to cubic-bezier(0.42, 0, 1, 1).
    return CubicBezierEasingFunction { 0.42, 0, 1, 1, "ease-in"_utf16 };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-out
EasingFunction EasingFunction::ease_out()
{
    // Equivalent to cubic-bezier(0, 0, 0.58, 1).
    return CubicBezierEasingFunction { 0, 0, 0.58, 1, "ease-out"_utf16 };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-in-out
EasingFunction EasingFunction::ease_in_out()
{
    // Equivalent to cubic-bezier(0.42, 0, 0.58, 1).
    return CubicBezierEasingFunction { 0.42, 0, 0.58, 1, "ease-in-out"_utf16 };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease
EasingFunction EasingFunction::ease()
{
    // Equivalent to cubic-bezier(0.25, 0.1, 0.25, 1).
    return CubicBezierEasingFunction { 0.25, 0.1, 0.25, 1, "ease"_utf16 };
}

EasingFunction EasingFunction::from_style_value(StyleValue const& style_value)
{
    if (style_value.is_easing()) {
        return style_value.as_easing().function().visit(
            [&](EasingStyleValue::Linear const& linear) -> EasingFunction {
                Vector<UnresolvedLinearEasingControlPoint> unresolved_control_points;

                for (auto const& control_point : linear.stops) {
                    double output = number_from_style_value(control_point.output, {});

                    Optional<double> input;
                    if (control_point.input)
                        input = Percentage::from_style_value(*control_point.input).as_fraction();

                    unresolved_control_points.append({ input, output });
                }

                // https://drafts.csswg.org/css-easing-2/#funcdef-linear
                // If an argument lacks a <percentage>, its input progress value is initially empty. This is corrected
                // at used value time by linear() canonicalization.
                auto resolved_control_points = canonicalize_linear_easing_function_control_points(move(unresolved_control_points));

                return LinearEasingFunction { resolved_control_points, linear.to_utf16_string(SerializationMode::ResolvedValue) };
            },
            [&](EasingStyleValue::CubicBezier const& cubic_bezier) -> EasingFunction {
                auto resolved_x1 = number_from_style_value(cubic_bezier.x1, {});
                auto resolved_y1 = number_from_style_value(cubic_bezier.y1, {});
                auto resolved_x2 = number_from_style_value(cubic_bezier.x2, {});
                auto resolved_y2 = number_from_style_value(cubic_bezier.y2, {});

                return CubicBezierEasingFunction { resolved_x1, resolved_y1, resolved_x2, resolved_y2, cubic_bezier.to_utf16_string(SerializationMode::Normal) };
            },
            [&](EasingStyleValue::Steps const& steps) -> EasingFunction {
                return StepsEasingFunction { int_from_style_value(steps.number_of_intervals), steps.position, steps.to_utf16_string(SerializationMode::ResolvedValue) };
            });
    }

    switch (style_value.to_keyword()) {
    case Keyword::Linear:
        return EasingFunction::linear();
    case Keyword::EaseIn:
        return EasingFunction::ease_in();
    case Keyword::EaseOut:
        return EasingFunction::ease_out();
    case Keyword::EaseInOut:
        return EasingFunction::ease_in_out();
    case Keyword::Ease:
        return EasingFunction::ease();
    default: {
        VERIFY_NOT_REACHED();
    }
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> EasingFunction::to_style_value() const
{
    auto const& serialized = to_utf16_string();
    if (serialized == "linear"_utf16)
        return KeywordStyleValue::create(Keyword::Linear);
    if (serialized == "ease"_utf16)
        return KeywordStyleValue::create(Keyword::Ease);
    if (serialized == "ease-in"_utf16)
        return KeywordStyleValue::create(Keyword::EaseIn);
    if (serialized == "ease-out"_utf16)
        return KeywordStyleValue::create(Keyword::EaseOut);
    if (serialized == "ease-in-out"_utf16)
        return KeywordStyleValue::create(Keyword::EaseInOut);

    return visit(
        [](LinearEasingFunction const& linear) -> NonnullRefPtr<StyleValue const> {
            Vector<EasingStyleValue::Linear::Stop> stops;
            for (auto const& point : linear.control_points) {
                auto input = PercentageStyleValue::create(Percentage { point.input * 100 });
                stops.append({ NumberStyleValue::create(point.output), move(input) });
            }
            return EasingStyleValue::create(EasingStyleValue::Linear { move(stops) });
        },
        [](CubicBezierEasingFunction const& cubic_bezier) -> NonnullRefPtr<StyleValue const> {
            return EasingStyleValue::create(EasingStyleValue::CubicBezier {
                NumberStyleValue::create(cubic_bezier.x1),
                NumberStyleValue::create(cubic_bezier.y1),
                NumberStyleValue::create(cubic_bezier.x2),
                NumberStyleValue::create(cubic_bezier.y2),
            });
        },
        [](StepsEasingFunction const& steps) -> NonnullRefPtr<StyleValue const> {
            return EasingStyleValue::create(EasingStyleValue::Steps {
                IntegerStyleValue::create(steps.interval_count),
                steps.position,
            });
        });
}

double EasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    return visit(
        [&](auto const& function) {
            return function.evaluate_at(input_progress, before_flag);
        });
}

String EasingFunction::to_string() const
{
    return visit(
        [](auto const& function) {
            return function.stringified.to_utf8();
        });
}

Utf16String const& EasingFunction::to_utf16_string() const
{
    return visit(
        [](auto const& function) -> Utf16String const& {
            return function.stringified;
        });
}

}
