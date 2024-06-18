/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
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

double EasingStyleValue::Function::evaluate_at(double input_progress, bool before_flag) const
{
    constexpr static auto cubic_bezier_at = [](double x1, double x2, double t) {
        auto a = 1.0 - 3.0 * x2 + 3.0 * x1;
        auto b = 3.0 * x2 - 6.0 * x1;
        auto c = 3.0 * x1;

        auto t2 = t * t;
        auto t3 = t2 * t;

        return (a * t3) + (b * t2) + (c * t);
    };

    return visit(
        [&](Linear const&) { return input_progress; },
        [&](CubicBezier const& bezier) {
            auto const& [x1, y1, x2, y2, cached_x_samples] = bezier;

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
                auto x = cubic_bezier_at(bezier.x1, bezier.x2, t);
                auto y = cubic_bezier_at(bezier.y1, bezier.y2, t);
                return CubicBezier::CachedSample { x, y, t };
            };

            if (cached_x_samples.is_empty())
                cached_x_samples.append(solve(0.));

            size_t nearby_index = 0;
            if (auto found = binary_search(cached_x_samples, x, &nearby_index, [](auto x, auto& sample) {
                    if (x > sample.x)
                        return 1;
                    if (x < sample.x)
                        return -1;
                    return 0;
                }))
                return found->y;

            if (nearby_index == cached_x_samples.size() || nearby_index + 1 == cached_x_samples.size()) {
                // Produce more samples until we have enough.
                auto last_t = cached_x_samples.last().t;
                auto last_x = cached_x_samples.last().x;
                while (last_x <= x && last_t < 1.0) {
                    last_t += 1. / 60.;
                    auto solution = solve(last_t);
                    cached_x_samples.append(solution);
                    last_x = solution.x;
                }

                if (auto found = binary_search(cached_x_samples, x, &nearby_index, [](auto x, auto& sample) {
                        if (x > sample.x)
                            return 1;
                        if (x < sample.x)
                            return -1;
                        return 0;
                    }))
                    return found->y;
            }

            // We have two samples on either side of the x value we want, so we can linearly interpolate between them.
            auto& sample1 = cached_x_samples[nearby_index];
            auto& sample2 = cached_x_samples[nearby_index + 1];
            auto factor = (x - sample1.x) / (sample2.x - sample1.x);
            return sample1.y + factor * (sample2.y - sample1.y);
        },
        [&](Steps const& steps) {
            // https://www.w3.org/TR/css-easing-1/#step-easing-algo
            // 1. Calculate the current step as floor(input progress value × steps).
            auto [number_of_steps, position] = steps;
            auto current_step = floor(input_progress * number_of_steps);

            // 2. If the step position property is one of:
            //    - jump-start,
            //    - jump-both,
            //    increment current step by one.
            if (position == Steps::Position::JumpStart || position == Steps::Position::JumpBoth)
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
            auto jumps = steps.number_of_intervals;
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
        });
}

String EasingStyleValue::Function::to_string() const
{
    StringBuilder builder;
    visit(
        [&](Linear const& linear) {
            builder.append("linear"sv);
            if (!linear.stops.is_empty()) {
                builder.append('(');

                bool first = true;
                for (auto const& stop : linear.stops) {
                    if (!first)
                        builder.append(", "sv);
                    first = false;
                    builder.appendff("{}"sv, stop.offset);
                    if (stop.position.has_value())
                        builder.appendff(" {}"sv, stop.position.value());
                }

                builder.append(')');
            }
        },
        [&](CubicBezier const& bezier) {
            if (bezier == CubicBezier::ease()) {
                builder.append("ease"sv);
            } else if (bezier == CubicBezier::ease_in()) {
                builder.append("ease-in"sv);
            } else if (bezier == CubicBezier::ease_out()) {
                builder.append("ease-out"sv);
            } else if (bezier == CubicBezier::ease_in_out()) {
                builder.append("ease-in-out"sv);
            } else {
                builder.appendff("cubic-bezier({}, {}, {}, {})", bezier.x1, bezier.y1, bezier.x2, bezier.y2);
            }
        },
        [&](Steps const& steps) {
            if (steps == Steps::step_start()) {
                builder.append("step-start"sv);
            } else if (steps == Steps::step_end()) {
                builder.append("step-end"sv);
            } else {
                auto position = [&] -> Optional<StringView> {
                    switch (steps.position) {
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
                    builder.appendff("steps({}, {})", steps.number_of_intervals, position.value());
                } else {
                    builder.appendff("steps({})", steps.number_of_intervals);
                }
            }
        });
    return MUST(builder.to_string());
}

}
