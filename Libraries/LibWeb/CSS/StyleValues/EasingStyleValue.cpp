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

// https://drafts.csswg.org/css-easing/#linear-easing-function-serializing
String EasingStyleValue::Linear::to_string(SerializationMode mode) const
{
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
    return MUST(String::formatted("cubic-bezier({}, {}, {}, {})", x1->to_string(mode), y1->to_string(mode), x2->to_string(mode), y2->to_string(mode)));
}

// https://drafts.csswg.org/css-easing/#steps-serialization
String EasingStyleValue::Steps::to_string(SerializationMode mode) const
{
    auto position = [&] -> Optional<StringView> {
        if (first_is_one_of(this->position, StepPosition::JumpEnd, StepPosition::End))
            return {};
        return CSS::to_string(this->position);
    }();

    if (position.has_value()) {
        return MUST(String::formatted("steps({}, {})", number_of_intervals->to_string(mode), position.value()));
    }

    return MUST(String::formatted("steps({})", number_of_intervals->to_string(mode)));
}

String EasingStyleValue::Function::to_string(SerializationMode mode) const
{
    return visit(
        [&](auto const& curve) {
            return curve.to_string(mode);
        });
}

ValueComparingNonnullRefPtr<StyleValue const> EasingStyleValue::absolutized(ComputationContext const& computation_context, PropertyComputationDependencies& property_computation_dependencies) const
{
    auto const& absolutized_function = m_function.visit(
        [&](Linear const& linear) -> Function {
            Vector<Linear::Stop> absolutized_stops;

            for (auto stop : linear.stops) {
                RefPtr<StyleValue const> absolutized_input;

                if (stop.input)
                    absolutized_input = stop.input->absolutized(computation_context, property_computation_dependencies);

                absolutized_stops.append({ stop.output->absolutized(computation_context, property_computation_dependencies), absolutized_input });
            }

            return Linear { absolutized_stops };
        },
        [&](CubicBezier const& cubic_bezier) -> Function {
            return CubicBezier {
                cubic_bezier.x1->absolutized(computation_context, property_computation_dependencies),
                cubic_bezier.y1->absolutized(computation_context, property_computation_dependencies),
                cubic_bezier.x2->absolutized(computation_context, property_computation_dependencies),
                cubic_bezier.y2->absolutized(computation_context, property_computation_dependencies)
            };
        },
        [&](Steps const& steps) -> Function {
            return Steps {
                steps.number_of_intervals->absolutized(computation_context, property_computation_dependencies),
                steps.position
            };
        });

    return EasingStyleValue::create(absolutized_function);
}

}
