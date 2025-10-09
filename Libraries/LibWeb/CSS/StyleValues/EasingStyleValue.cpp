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

namespace Web::CSS {

// https://drafts.csswg.org/css-easing-1/#valdef-easing-function-linear
EasingStyleValue::Linear EasingStyleValue::Linear::identity()
{
    static Linear linear { { { 0, {} }, { 1, {} } } };
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
    static Steps steps { 1, StepPosition::Start };
    return steps;
}

EasingStyleValue::Steps EasingStyleValue::Steps::step_end()
{
    static Steps steps { 1, StepPosition::End };
    return steps;
}

// https://drafts.csswg.org/css-easing/#linear-easing-function-serializing
String EasingStyleValue::Linear::to_string(SerializationMode) const
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
        if (stop.input.has_value()) {
            builder.appendff(" {}%", stop.input.value() * 100);
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
        auto x1_value = x1;
        auto y1_value = y1;
        auto x2_value = x2;
        auto y2_value = y2;
        if (mode == SerializationMode::ResolvedValue) {
            x1_value = clamp(x1_value.resolved({}).value_or(0.0), 0.0, 1.0);
            x2_value = clamp(x2_value.resolved({}).value_or(0.0), 0.0, 1.0);
            y1_value = y1_value.resolved({}).value_or(0.0);
            y2_value = y2_value.resolved({}).value_or(0.0);
        }
        builder.appendff("cubic-bezier({}, {}, {}, {})",
            x1_value.to_string(mode), y1_value.to_string(mode), x2_value.to_string(mode), y2_value.to_string(mode));
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
        auto intervals = number_of_intervals;
        if (mode == SerializationMode::ResolvedValue) {
            auto resolved_value = number_of_intervals.resolved({}).value_or(1);
            intervals = max(resolved_value, this->position == StepPosition::JumpNone ? 2 : 1);
        }
        if (position.has_value()) {
            builder.appendff("steps({}, {})", intervals.to_string(mode), position.value());
        } else {
            builder.appendff("steps({})", intervals.to_string(mode));
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

}
