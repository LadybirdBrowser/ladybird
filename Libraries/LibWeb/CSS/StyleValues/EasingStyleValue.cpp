/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EasingStyleValue.h"
#include <AK/BinarySearch.h>
#include <AK/StringBuilder.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-easing-1/#valdef-easing-function-linear
EasingStyleValue::Linear EasingStyleValue::Linear::identity()
{
    static Linear linear { { { 0, {}, false }, { 1, {}, false } } };
    return linear;
}

// NOTE: Magic cubic bezier values from https://www.w3.org/TR/css-easing-1/#valdef-cubic-bezier-easing-function-ease

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease()
{
    static CubicBezier bezier { 0.25, 0.1, 0.25, 1.0 };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_in()
{
    static CubicBezier bezier { 0.42, 0.0, 1.0, 1.0 };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_out()
{
    static CubicBezier bezier { 0.0, 0.0, 0.58, 1.0 };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_in_out()
{
    static CubicBezier bezier { 0.42, 0.0, 0.58, 1.0 };
    return bezier;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_start()
{
    static Steps steps { 1, Steps::Position::Start };
    return steps;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_end()
{
    static Steps steps { 1, Steps::Position::End };
    return steps;
}

bool EasingStyleValue::CubicBezier::operator==(Web::CSS::EasingStyleValue::CubicBezier const& other) const
{
    return x1 == other.x1 && y1 == other.y1 && x2 == other.x2 && y2 == other.y2;
}

// https://drafts.csswg.org/css-easing/#linear-canonicalization
EasingStyleValue::Linear::Linear(Vector<EasingStyleValue::Linear::Stop> stops)
{
    // To canonicalize a linear() function’s control points, perform the following:

    // 1. If the first control point lacks an input progress value, set its input progress value to 0.
    if (!stops.first().input.has_value())
        stops.first().input = 0;

    // 2. If the last control point lacks an input progress value, set its input progress value to 1.
    if (!stops.last().input.has_value())
        stops.last().input = 1;

    // 3. If any control point has an input progress value that is less than
    // the input progress value of any preceding control point,
    // set its input progress value to the largest input progress value of any preceding control point.
    double largest_input = 0;
    for (auto stop : stops) {
        if (stop.input.has_value()) {
            if (stop.input.value() < largest_input) {
                stop.input = largest_input;
            } else {
                largest_input = stop.input.value();
            }
        }
    }

    // 4. If any control point still lacks an input progress value,
    // then for each contiguous run of such control points,
    // set their input progress values so that they are evenly spaced
    // between the preceding and following control points with input progress values.
    Optional<size_t> run_start_idx;
    for (size_t idx = 0; idx < stops.size(); idx++) {
        auto stop = stops[idx];
        if (stop.input.has_value() && run_start_idx.has_value()) {
            // Note: this stop is immediately after a run
            //       set inputs of [start, idx-1] stops to be evenly spaced between start-1 and idx
            auto start_input = stops[run_start_idx.value() - 1].input.value();
            auto end_input = stops[idx].input.value();
            auto run_stop_count = idx - run_start_idx.value() + 1;
            auto delta = (end_input - start_input) / run_stop_count;
            for (size_t run_idx = 0; run_idx < run_stop_count; run_idx++) {
                stops[run_idx + run_start_idx.value() - 1].input = start_input + delta * run_idx;
            }
            run_start_idx = {};
        } else if (!stop.input.has_value() && !run_start_idx.has_value()) {
            // Note: this stop is the start of a run
            run_start_idx = idx;
        }
    }

    this->stops = move(stops);
}

// https://drafts.csswg.org/css-easing/#linear-easing-function-output
double EasingStyleValue::Linear::evaluate_at(double input_progress, bool before_flag) const
{
    // To calculate linear easing output progress for a given linear easing function func,
    // an input progress value inputProgress, and an optional before flag (defaulting to false),
    // perform the following:

    // 1. Let points be func’s control points.
    // 2. If points holds only a single item, return the output progress value of that item.
    if (stops.size() == 1)
        return stops[0].output;

    // 3. If inputProgress matches the input progress value of the first point in points,
    // and the before flag is true, return the first point’s output progress value.
    if (input_progress == stops[0].input.value() && before_flag)
        return stops[0].output;

    // 4. If inputProgress matches the input progress value of at least one point in points,
    // return the output progress value of the last such point.
    auto maybe_match = stops.last_matching([&](auto& stop) { return input_progress == stop.input.value(); });
    if (maybe_match.has_value())
        return maybe_match->output;

    // 5. Otherwise, find two control points in points, A and B, which will be used for interpolation:
    Stop A;
    Stop B;

    if (input_progress < stops[0].input.value()) {
        // 1. If inputProgress is smaller than any input progress value in points,
        // let A and B be the first two items in points.
        // If A and B have the same input progress value, return A’s output progress value.
        A = stops[0];
        B = stops[1];
        if (A.input == B.input)
            return A.output;
    } else if (input_progress > stops.last().input.value()) {
        // 2. If inputProgress is larger than any input progress value in points,
        // let A and B be the last two items in points.
        // If A and B have the same input progress value, return B’s output progress value.
        A = stops[stops.size() - 2];
        B = stops[stops.size() - 1];
        if (A.input == B.input)
            return B.output;
    } else {
        // 3. Otherwise, let A be the last control point whose input progress value is smaller than inputProgress,
        // and let B be the first control point whose input progress value is larger than inputProgress.
        A = stops.last_matching([&](auto& stop) { return stop.input.value() < input_progress; }).value();
        B = stops.first_matching([&](auto& stop) { return stop.input.value() > input_progress; }).value();
    }

    // 6. Linearly interpolate (or extrapolate) inputProgress along the line defined by A and B, and return the result.
    auto factor = (input_progress - A.input.value()) / (B.input.value() - A.input.value());
    return A.output + factor * (B.output - A.output);
}

// https://drafts.csswg.org/css-easing/#linear-easing-function-serializing
String EasingStyleValue::Linear::to_string() const
{
    // The linear keyword is serialized as itself.
    if (*this == identity())
        return "linear"_string;

    // To serialize a linear() function:
    // 1. Let s be the string "linear(".
    StringBuilder builder;
    builder.append("linear("sv);

    // 2. Serialize each control point of the function,
    // concatenate the results using the separator ", ",
    // and append the result to s.
    bool first = true;
    for (auto stop : stops) {
        if (first) {
            first = false;
        } else {
            builder.append(", "sv);
        }

        // To serialize a linear() control point:
        // 1. Let s be the serialization, as a <number>, of the control point’s output progress value.
        builder.appendff("{}", stop.output);

        // 2. If the control point originally lacked an input progress value, return s.
        // 3. Otherwise, append " " (U+0020 SPACE) to s,
        // then serialize the control point’s input progress value as a <percentage> and append it to s.
        if (stop.had_explicit_input) {
            builder.appendff(" {}%", stop.input.value() * 100);
        }

        // 4. Return s.
    }

    // 4. Append ")" to s, and return it.
    builder.append(')');
    return MUST(builder.to_string());
}

double EasingStyleValue::CubicBezier::evaluate_at(double input_progress, bool) const
{
    constexpr static auto cubic_bezier_at = [](double x1, double x2, double t) {
        auto a = 1.0 - 3.0 * x2 + 3.0 * x1;
        auto b = 3.0 * x2 - 6.0 * x1;
        auto c = 3.0 * x1;

        auto t2 = t * t;
        auto t3 = t2 * t;

        return (a * t3) + (b * t2) + (c * t);
    };

    // https://www.w3.org/TR/css-easing-1/#cubic-bezier-algo
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
        return CubicBezier::CachedSample { x, y, t };
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

// https://drafts.csswg.org/css-easing/#bezier-serialization
String EasingStyleValue::CubicBezier::to_string() const
{
    StringBuilder builder;
    if (*this == CubicBezier::ease()) {
        builder.append("ease"sv);
    } else if (*this == CubicBezier::ease_in()) {
        builder.append("ease-in"sv);
    } else if (*this == CubicBezier::ease_out()) {
        builder.append("ease-out"sv);
    } else if (*this == CubicBezier::ease_in_out()) {
        builder.append("ease-in-out"sv);
    } else {
        builder.appendff("cubic-bezier({}, {}, {}, {})", x1, y1, x2, y2);
    }
    return MUST(builder.to_string());
}

double EasingStyleValue::Steps::evaluate_at(double input_progress, bool before_flag) const
{
    // https://www.w3.org/TR/css-easing-1/#step-easing-algo
    // 1. Calculate the current step as floor(input progress value × steps).
    auto current_step = floor(input_progress * number_of_intervals);

    // 2. If the step position property is one of:
    //    - jump-start,
    //    - jump-both,
    //    increment current step by one.
    if (position == Steps::Position::JumpStart || position == Steps::Position::Start || position == Steps::Position::JumpBoth)
        current_step += 1;

    // 3. If both of the following conditions are true:
    //    - the before flag is set, and
    //    - input progress value × steps mod 1 equals zero (that is, if input progress value × steps is integral), then
    //    decrement current step by one.
    auto step_progress = input_progress * number_of_intervals;
    if (before_flag && trunc(step_progress) == step_progress)
        current_step -= 1;

    // 4. If input progress value ≥ 0 and current step < 0, let current step be zero.
    if (input_progress >= 0.0 && current_step < 0.0)
        current_step = 0.0;

    // 5. Calculate jumps based on the step position as follows:

    //    jump-start or jump-end -> steps
    //    jump-none -> steps - 1
    //    jump-both -> steps + 1
    auto jumps = number_of_intervals;
    if (position == Steps::Position::JumpNone) {
        jumps--;
    } else if (position == Steps::Position::JumpBoth) {
        jumps++;
    }

    // 6. If input progress value ≤ 1 and current step > jumps, let current step be jumps.
    if (input_progress <= 1.0 && current_step > jumps)
        current_step = jumps;

    // 7. The output progress value is current step / jumps.
    return current_step / jumps;
}

// https://drafts.csswg.org/css-easing/#steps-serialization
String EasingStyleValue::Steps::to_string() const
{
    StringBuilder builder;
    // Unlike the other easing function keywords, step-start and step-end do not serialize as themselves.
    // Instead, they serialize as "steps(1, start)" and "steps(1)", respectively.
    if (*this == Steps::step_start()) {
        builder.append("steps(1, start)"sv);
    } else if (*this == Steps::step_end()) {
        builder.append("steps(1)"sv);
    } else {
        auto position = [&] -> Optional<StringView> {
            switch (this->position) {
            case Steps::Position::JumpStart:
                return "jump-start"sv;
            case Steps::Position::JumpNone:
                return "jump-none"sv;
            case Steps::Position::JumpBoth:
                return "jump-both"sv;
            case Steps::Position::Start:
                return "start"sv;
            default:
                return {};
            }
        }();
        if (position.has_value()) {
            builder.appendff("steps({}, {})", number_of_intervals, position.value());
        } else {
            builder.appendff("steps({})", number_of_intervals);
        }
    }
    return MUST(builder.to_string());
}

double EasingStyleValue::Function::evaluate_at(double input_progress, bool before_flag) const
{
    return visit(
        [&](auto const& curve) {
            return curve.evaluate_at(input_progress, before_flag);
        });
}

String EasingStyleValue::Function::to_string() const
{
    return visit(
        [&](auto const& curve) {
            return curve.to_string();
        });
}

}
