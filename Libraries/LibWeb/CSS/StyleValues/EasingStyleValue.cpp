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
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-easing-1/#valdef-easing-function-linear
EasingStyleValue::Linear EasingStyleValue::Linear::identity()
{
    static Linear linear { { { NumberStyleValue::create(0), {} }, { NumberStyleValue::create(1), {} } } };
    return linear;
}

// NOTE: Magic cubic bezier values from https://www.w3.org/TR/css-easing-1/#valdef-cubic-bezier-easing-function-ease

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease()
{
    static CubicBezier bezier { NumberStyleValue::create(0.25), NumberStyleValue::create(0.1), NumberStyleValue::create(0.25), NumberStyleValue::create(1.0) };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_in()
{
    static CubicBezier bezier { NumberStyleValue::create(0.42), NumberStyleValue::create(0.0), NumberStyleValue::create(1.0), NumberStyleValue::create(1.0) };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_out()
{
    static CubicBezier bezier { NumberStyleValue::create(0.0), NumberStyleValue::create(0.0), NumberStyleValue::create(0.58), NumberStyleValue::create(1.0) };
    return bezier;
}

EasingStyleValue::CubicBezier EasingStyleValue::CubicBezier::ease_in_out()
{
    static CubicBezier bezier { NumberStyleValue::create(0.42), NumberStyleValue::create(0.0), NumberStyleValue::create(0.58), NumberStyleValue::create(1.0) };
    return bezier;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_start()
{
    static Steps steps { IntegerStyleValue::create(1), StepPosition::Start };
    return steps;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_end()
{
    static Steps steps { IntegerStyleValue::create(1), StepPosition::End };
    return steps;
}

// https://drafts.csswg.org/css-easing/#linear-easing-function-serializing
String EasingStyleValue::Linear::to_string(SerializationMode mode) const
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
        builder.appendff(stop.output->to_string(mode));

        // 2. If the control point originally lacked an input progress value, return s.
        // 3. Otherwise, append " " (U+0020 SPACE) to s,
        // then serialize the control point’s input progress value as a <percentage> and append it to s.
        if (stop.input) {
            builder.appendff(" {}", stop.input->to_string(mode));
        }

        // 4. Return s.
    }

    // 4. Append ")" to s, and return it.
    builder.append(')');
    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-easing/#bezier-serialization
String EasingStyleValue::CubicBezier::to_string(SerializationMode mode) const
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
        builder.appendff("cubic-bezier({}, {}, {}, {})", x1->to_string(mode), y1->to_string(mode), x2->to_string(mode), y2->to_string(mode));
    }
    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-easing/#steps-serialization
String EasingStyleValue::Steps::to_string(SerializationMode mode) const
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
            if (first_is_one_of(this->position, StepPosition::JumpEnd, StepPosition::End))
                return {};
            return CSS::to_string(this->position);
        }();
        if (position.has_value()) {
            builder.appendff("steps({}, {})", number_of_intervals->to_string(mode), position.value());
        } else {
            builder.appendff("steps({})", number_of_intervals->to_string(mode));
        }
    }
    return MUST(builder.to_string());
}

String EasingStyleValue::Function::to_string(SerializationMode mode) const
{
    return visit(
        [&](auto const& curve) {
            return curve.to_string(mode);
        });
}

ValueComparingNonnullRefPtr<StyleValue const> EasingStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_function = m_function.visit(
        [&](Linear const& linear) -> Function {
            Vector<Linear::Stop> absolutized_stops;

            for (auto stop : linear.stops) {
                RefPtr<StyleValue const> absolutized_input;

                if (stop.input)
                    absolutized_input = stop.input->absolutized(computation_context);

                absolutized_stops.append({ stop.output->absolutized(computation_context), absolutized_input });
            }

            return Linear { absolutized_stops };
        },
        [&](CubicBezier const& cubic_bezier) -> Function {
            return CubicBezier {
                cubic_bezier.x1->absolutized(computation_context),
                cubic_bezier.y1->absolutized(computation_context),
                cubic_bezier.x2->absolutized(computation_context),
                cubic_bezier.y2->absolutized(computation_context)
            };
        },
        [&](Steps const& steps) -> Function {
            return Steps {
                steps.number_of_intervals->absolutized(computation_context),
                steps.position
            };
        });

    return EasingStyleValue::create(absolutized_function);
}

}
