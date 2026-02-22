/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EasingFunction.h"
#include <AK/BinarySearch.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-easing/#linear-easing-function-output
double LinearEasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    // To calculate linear easing output progress for a given linear easing function func,
    // an input progress value inputProgress, and an optional before flag (defaulting to false),
    // perform the following:

    // 1. Let points be func’s control points.

    // 2. If points holds only a single item, return the output progress value of that item.
    if (control_points.size() == 1)
        return control_points[0].output;

    // 3. If inputProgress matches the input progress value of the first point in points,
    // and the before flag is true, return the first point’s output progress value.
    if (input_progress == control_points[0].input.value() && before_flag)
        return control_points[0].output;

    // 4. If inputProgress matches the input progress value of at least one point in points,
    // return the output progress value of the last such point.
    auto maybe_match = control_points.last_matching([&](auto& stop) { return input_progress == stop.input; });
    if (maybe_match.has_value())
        return maybe_match->output;

    // 5. Otherwise, find two control points in points, A and B, which will be used for interpolation:
    ControlPoint A;
    ControlPoint B;

    if (input_progress < control_points[0].input.value()) {
        // 1. If inputProgress is smaller than any input progress value in points,
        // let A and B be the first two items in points.
        // If A and B have the same input progress value, return A’s output progress value.
        A = control_points[0];
        B = control_points[1];
        if (A.input == B.input.value())
            return A.output;
    } else if (input_progress > control_points.last().input.value()) {
        // 2. If inputProgress is larger than any input progress value in points,
        // let A and B be the last two items in points.
        // If A and B have the same input progress value, return B’s output progress value.
        A = control_points[control_points.size() - 2];
        B = control_points[control_points.size() - 1];
        if (A.input == B.input.value())
            return B.output;
    } else {
        // 3. Otherwise, let A be the last control point whose input progress value is smaller than inputProgress,
        // and let B be the first control point whose input progress value is larger than inputProgress.
        A = control_points.last_matching([&](ControlPoint const& stop) { return stop.input.value() < input_progress; }).value();
        B = control_points.first_matching([&](ControlPoint const& stop) { return stop.input.value() > input_progress; }).value();
    }

    // 6. Linearly interpolate (or extrapolate) inputProgress along the line defined by A and B, and return the result.
    auto factor = (input_progress - A.input.value()) / (B.input.value() - A.input.value());
    return A.output + factor * (B.output - A.output);
}

// https://www.w3.org/TR/css-easing-1/#cubic-bezier-algo
double CubicBezierEasingFunction::evaluate_at(double input_progress, bool) const
{
    constexpr static auto cubic_bezier_at = [](double x1, double x2, double t) {
        auto a = 1.0 - 3.0 * x2 + 3.0 * x1;
        auto b = 3.0 * x2 - 6.0 * x1;
        auto c = 3.0 * x1;

        auto t2 = t * t;
        auto t3 = t2 * t;

        return (a * t3) + (b * t2) + (c * t);
    };

    // For input progress values outside the range [0, 1], the curve is extended infinitely using tangent of the curve
    // at the closest endpoint as follows:

    // - For input progress values less than zero,
    if (input_progress < 0.0) {
        // 1. If the x value of P1 is greater than zero, use a straight line that passes through P1 and P0 as the
        //    tangent.
        if (x1 > 0.0)
            return y1 / x1 * input_progress;

        // 2. Otherwise, if the x value of P2 is greater than zero, use a straight line that passes through P2 and P0 as
        //    the tangent.
        if (x2 > 0.0)
            return y2 / x2 * input_progress;

        // 3. Otherwise, let the output progress value be zero for all input progress values in the range [-∞, 0).
        return 0.0;
    }

    // - For input progress values greater than one,
    if (input_progress > 1.0) {
        // 1. If the x value of P2 is less than one, use a straight line that passes through P2 and P3 as the tangent.
        if (x2 < 1.0)
            return (1.0 - y2) / (1.0 - x2) * (input_progress - 1.0) + 1.0;

        // 2. Otherwise, if the x value of P1 is less than one, use a straight line that passes through P1 and P3 as the
        //    tangent.
        if (x1 < 1.0)
            return (1.0 - y1) / (1.0 - x1) * (input_progress - 1.0) + 1.0;

        // 3. Otherwise, let the output progress value be one for all input progress values in the range (1, ∞].
        return 1.0;
    }

    // Note: The spec does not specify the precise algorithm for calculating values in the range [0, 1]:
    //       "The evaluation of this curve is covered in many sources such as [FUND-COMP-GRAPHICS]."

    auto x = input_progress;

    auto solve = [&](auto t) {
        auto x = cubic_bezier_at(x1, x2, t);
        auto y = cubic_bezier_at(y1, y2, t);
        return CachedSample { x, y, t };
    };

    if (m_cached_x_samples.is_empty())
        m_cached_x_samples.append(solve(0.));

    size_t nearby_index = 0;
    if (auto found = binary_search(m_cached_x_samples, x, &nearby_index, [](auto x, auto& sample) {
            if (x - sample.x >= NumericLimits<double>::epsilon())
                return 1;
            if (x - sample.x <= NumericLimits<double>::epsilon())
                return -1;
            return 0;
        }))
        return found->y;

    if (nearby_index == m_cached_x_samples.size() || nearby_index + 1 == m_cached_x_samples.size()) {
        // Produce more samples until we have enough.
        auto last_t = m_cached_x_samples.last().t;
        auto last_x = m_cached_x_samples.last().x;
        while (last_x <= x && last_t < 1.0) {
            last_t += 1. / 60.;
            auto solution = solve(last_t);
            m_cached_x_samples.append(solution);
            last_x = solution.x;
        }

        if (auto found = binary_search(m_cached_x_samples, x, &nearby_index, [](auto x, auto& sample) {
                if (x - sample.x >= NumericLimits<double>::epsilon())
                    return 1;
                if (x - sample.x <= NumericLimits<double>::epsilon())
                    return -1;
                return 0;
            }))
            return found->y;
    }

    // We have two samples on either side of the x value we want, so we can linearly interpolate between them.
    auto& sample1 = m_cached_x_samples[nearby_index];
    auto& sample2 = m_cached_x_samples[nearby_index + 1];
    auto factor = (x - sample1.x) / (sample2.x - sample1.x);
    return sample1.y + factor * (sample2.y - sample1.y);
}

// https://www.w3.org/TR/css-easing-1/#step-easing-algo
double StepsEasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    auto current_step = floor(input_progress * interval_count);

    // 2. If the step position property is one of:
    //    - jump-start,
    //    - jump-both,
    //    increment current step by one.
    if (position == StepPosition::JumpStart || position == StepPosition::Start || position == StepPosition::JumpBoth)
        current_step += 1;

    // 3. If both of the following conditions are true:
    //    - the before flag is set, and
    //    - input progress value × steps mod 1 equals zero (that is, if input progress value × steps is integral), then
    //    decrement current step by one.
    auto step_progress = input_progress * interval_count;
    if (before_flag && trunc(step_progress) == step_progress)
        current_step -= 1;

    // 4. If input progress value ≥ 0 and current step < 0, let current step be zero.
    if (input_progress >= 0.0 && current_step < 0.0)
        current_step = 0.0;

    // 5. Calculate jumps based on the step position as follows:

    //    jump-start or jump-end -> steps
    //    jump-none -> steps - 1
    //    jump-both -> steps + 1
    auto jumps = interval_count;
    if (position == StepPosition::JumpNone) {
        jumps--;
    } else if (position == StepPosition::JumpBoth) {
        jumps++;
    }

    // 6. If input progress value ≤ 1 and current step > jumps, let current step be jumps.
    if (input_progress <= 1.0 && current_step > jumps)
        current_step = jumps;

    // 7. The output progress value is current step / jumps.
    return current_step / jumps;
}

// https://drafts.csswg.org/css-easing/#linear-canonicalization
static Vector<LinearEasingFunction::ControlPoint> canonicalize_linear_easing_function_control_points(Vector<LinearEasingFunction::ControlPoint> control_points)
{
    // To canonicalize a linear() function’s control points, perform the following:
    Vector<LinearEasingFunction::ControlPoint> canonicalized_control_points = control_points;

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

    return canonicalized_control_points;
}

// https://drafts.csswg.org/css-easing-2/#linear-easing-function
EasingFunction EasingFunction::linear()
{
    // Equivalent to linear(0, 1)
    return LinearEasingFunction { { { 0, 0 }, { 1, 1 } }, "linear"_string };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-in
EasingFunction EasingFunction::ease_in()
{
    // Equivalent to cubic-bezier(0.42, 0, 1, 1).
    return CubicBezierEasingFunction { 0.42, 0, 1, 1, "ease-in"_string };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-out
EasingFunction EasingFunction::ease_out()
{
    // Equivalent to cubic-bezier(0, 0, 0.58, 1).
    return CubicBezierEasingFunction { 0, 0, 0.58, 1, "ease-out"_string };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease-in-out
EasingFunction EasingFunction::ease_in_out()
{
    // Equivalent to cubic-bezier(0.42, 0, 0.58, 1).
    return CubicBezierEasingFunction { 0.42, 0, 0.58, 1, "ease-in-out"_string };
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease
EasingFunction EasingFunction::ease()
{
    // Equivalent to cubic-bezier(0.25, 0.1, 0.25, 1).
    return CubicBezierEasingFunction { 0.25, 0.1, 0.25, 1, "ease"_string };
}

EasingFunction EasingFunction::from_style_value(StyleValue const& style_value)
{
    auto const resolve_percentage = [](StyleValue const& style_value) {
        if (style_value.is_percentage())
            return style_value.as_percentage().percentage().as_fraction();

        if (style_value.is_calculated())
            return style_value.as_calculated().resolve_percentage({})->as_fraction();

        VERIFY_NOT_REACHED();
    };

    if (style_value.is_easing()) {
        return style_value.as_easing().function().visit(
            [&](EasingStyleValue::Linear const& linear) -> EasingFunction {
                Vector<LinearEasingFunction::ControlPoint> resolved_control_points;

                for (auto const& control_point : linear.stops) {
                    double output = number_from_style_value(control_point.output, {});

                    Optional<double> input;
                    if (control_point.input)
                        input = resolve_percentage(*control_point.input);

                    resolved_control_points.append({ input, output });
                }

                // https://drafts.csswg.org/css-easing-2/#funcdef-linear
                // If an argument lacks a <percentage>, its input progress value is initially empty. This is corrected
                // at used value time by linear() canonicalization.
                resolved_control_points = canonicalize_linear_easing_function_control_points(resolved_control_points);

                return LinearEasingFunction { resolved_control_points, linear.to_string(SerializationMode::ResolvedValue) };
            },
            [&](EasingStyleValue::CubicBezier const& cubic_bezier) -> EasingFunction {
                auto resolved_x1 = number_from_style_value(cubic_bezier.x1, {});
                auto resolved_y1 = number_from_style_value(cubic_bezier.y1, {});
                auto resolved_x2 = number_from_style_value(cubic_bezier.x2, {});
                auto resolved_y2 = number_from_style_value(cubic_bezier.y2, {});

                return CubicBezierEasingFunction { resolved_x1, resolved_y1, resolved_x2, resolved_y2, cubic_bezier.to_string(SerializationMode::Normal) };
            },
            [&](EasingStyleValue::Steps const& steps) -> EasingFunction {
                return StepsEasingFunction { int_from_style_value(steps.number_of_intervals), steps.position, steps.to_string(SerializationMode::ResolvedValue) };
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
            return function.stringified;
        });
}

}
