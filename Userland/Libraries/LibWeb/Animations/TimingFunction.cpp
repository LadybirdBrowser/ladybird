/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibWeb/Animations/TimingFunction.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <math.h>

namespace Web::Animations {

// https://www.w3.org/TR/css-easing-1/#linear-easing-function
double LinearTimingFunction::operator()(double input_progress, bool) const
{
    return input_progress;
}

static double cubic_bezier_at(double x1, double x2, double t)
{
    auto a = 1.0 - 3.0 * x2 + 3.0 * x1;
    auto b = 3.0 * x2 - 6.0 * x1;
    auto c = 3.0 * x1;

    auto t2 = t * t;
    auto t3 = t2 * t;

    return (a * t3) + (b * t2) + (c * t);
}

// https://www.w3.org/TR/css-easing-1/#cubic-bezier-algo
double CubicBezierTimingFunction::operator()(double input_progress, bool) const
{
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
            if (x > sample.x)
                return 1;
            if (x < sample.x)
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
                if (x > sample.x)
                    return 1;
                if (x < sample.x)
                    return -1;
                return 0;
            }))
            return found->y;
    }

    // We have two samples on either side of the x value we want, so we can linearly interpolate between them.
    auto& sample1 = m_cached_x_samples[nearby_index];
    auto& sample2 = m_cached_x_samples[nearby_index + 1];
    auto factor = (x - sample1.x) / (sample2.x - sample1.x);
    return clamp(sample1.y + factor * (sample2.y - sample1.y), 0, 1);
}

// https://www.w3.org/TR/css-easing-1/#step-easing-algo
double StepsTimingFunction::operator()(double input_progress, bool before_flag) const
{
    // 1. Calculate the current step as floor(input progress value × steps).
    auto current_step = floor(input_progress * number_of_steps);

    // 2. If the step position property is one of:
    //    - jump-start,
    //    - jump-both,
    //    increment current step by one.
    if (jump_at_start)
        current_step += 1;

    // 3. If both of the following conditions are true:
    //    - the before flag is set, and
    //    - input progress value × steps mod 1 equals zero (that is, if input progress value × steps is integral), then
    //    decrement current step by one.
    auto step_progress = input_progress * number_of_steps;
    if (before_flag && trunc(step_progress) == step_progress)
        current_step -= 1;

    // 4. If input progress value ≥ 0 and current step < 0, let current step be zero.
    if (input_progress >= 0.0 && current_step < 0.0)
        current_step = 0.0;

    // 5. Calculate jumps based on the step position as follows:

    //    jump-start or jump-end -> steps
    //    jump-none -> steps - 1
    //    jump-both -> steps + 1
    double jumps;
    if (jump_at_start ^ jump_at_end)
        jumps = number_of_steps;
    else if (jump_at_start && jump_at_end)
        jumps = number_of_steps + 1;
    else
        jumps = number_of_steps - 1;

    // 6. If input progress value ≤ 1 and current step > jumps, let current step be jumps.
    if (input_progress <= 1.0 && current_step > jumps)
        current_step = jumps;

    // 7. The output progress value is current step / jumps.
    return current_step / jumps;
}

TimingFunction TimingFunction::from_easing_style_value(CSS::EasingStyleValue const& easing_value)
{
    switch (easing_value.easing_function()) {
    case CSS::EasingFunction::Linear:
        return Animations::linear_timing_function;
    case CSS::EasingFunction::Ease:
        return Animations::ease_timing_function;
    case CSS::EasingFunction::EaseIn:
        return Animations::ease_in_timing_function;
    case CSS::EasingFunction::EaseOut:
        return Animations::ease_out_timing_function;
    case CSS::EasingFunction::EaseInOut:
        return Animations::ease_in_out_timing_function;
    case CSS::EasingFunction::CubicBezier: {
        auto values = easing_value.values();
        return {
            Animations::CubicBezierTimingFunction {
                values[0]->as_number().number(),
                values[1]->as_number().number(),
                values[2]->as_number().number(),
                values[3]->as_number().number(),
            },
        };
    }
    case CSS::EasingFunction::Steps: {
        auto values = easing_value.values();
        auto jump_at_start = false;
        auto jump_at_end = true;

        if (values.size() > 1) {
            auto identifier = values[1]->to_identifier();
            switch (identifier) {
            case CSS::ValueID::JumpStart:
            case CSS::ValueID::Start:
                jump_at_start = true;
                jump_at_end = false;
                break;
            case CSS::ValueID::JumpEnd:
            case CSS::ValueID::End:
                jump_at_start = false;
                jump_at_end = true;
                break;
            case CSS::ValueID::JumpNone:
                jump_at_start = false;
                jump_at_end = false;
                break;
            default:
                break;
            }
        }

        return Animations::TimingFunction { Animations::StepsTimingFunction {
            .number_of_steps = static_cast<size_t>(max(values[0]->as_integer().integer(), !(jump_at_end && jump_at_start) ? 1 : 0)),
            .jump_at_start = jump_at_start,
            .jump_at_end = jump_at_end,
        } };
    }
    case CSS::EasingFunction::StepEnd:
        return Animations::TimingFunction { Animations::StepsTimingFunction {
            .number_of_steps = 1,
            .jump_at_start = false,
            .jump_at_end = true,
        } };
    case CSS::EasingFunction::StepStart:
        return Animations::TimingFunction { Animations::StepsTimingFunction {
            .number_of_steps = 1,
            .jump_at_start = true,
            .jump_at_end = false,
        } };
    default:
        return Animations::ease_timing_function;
    }
}

double TimingFunction::operator()(double input_progress, bool before_flag) const
{
    return function.visit([&](auto const& f) { return f(input_progress, before_flag); });
}

}
