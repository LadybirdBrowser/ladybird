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

EasingStyleValue::Steps EasingStyleValue::Steps::step_start() {
    static Steps steps { 1, Steps::Position::Start };
    return steps;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_end() {
    static Steps steps { 1, Steps::Position::End };
    return steps;
}

String EasingStyleValue::to_string() const
{
    StringBuilder builder;
    m_function.visit(
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
