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
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData const* EasingStyleValue::make_easing_data(Function const& function)
{
    auto retain = [](StyleValue const* value) {
        return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
    };
    return function.visit(
        [&](Linear const& linear) {
            Vector<StyleValueFFI::RetainedLinearEasingStop> stops;
            stops.ensure_capacity(linear.stops.size());
            for (auto const& stop : linear.stops)
                stops.unchecked_append({ { retain(stop.output.ptr()) }, { retain(stop.input.ptr()) } });
            return StyleValueFFI::rust_style_value_create_easing(0, stops.data(), stops.size(), nullptr, nullptr, nullptr, nullptr, nullptr, 0);
        },
        [&](CubicBezier const& bezier) {
            return StyleValueFFI::rust_style_value_create_easing(1, nullptr, 0, retain(bezier.x1.ptr()), retain(bezier.y1.ptr()), retain(bezier.x2.ptr()), retain(bezier.y2.ptr()), nullptr, 0);
        },
        [&](Steps const& steps) {
            return StyleValueFFI::rust_style_value_create_easing(2, nullptr, 0, nullptr, nullptr, nullptr, nullptr, retain(steps.number_of_intervals.ptr()), to_underlying(steps.position));
        });
}

EasingStyleValue::EasingStyleValue(StyleValueFFI::StyleValueData const* data)
    : StyleValueWithDefaultOperators(Type::Easing, data)
    , m_function([&]() -> Function {
        auto adopt = [](auto const& retained) -> ValueComparingNonnullRefPtr<StyleValue const> {
            auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
        };
        auto adopt_optional = [](auto const& retained) -> ValueComparingRefPtr<StyleValue const> {
            auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
            if (!child_data)
                return nullptr;
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data));
        };
        auto const& easing = data->easing;
        switch (easing.kind) {
        case 0: {
            Vector<Linear::Stop> stops;
            stops.ensure_capacity(easing.linear_stops.length);
            for (size_t i = 0; i < easing.linear_stops.length; ++i) {
                auto const& stop = easing.linear_stops.pointer[i];
                stops.unchecked_append({ adopt(stop.output), adopt_optional(stop.input) });
            }
            return Linear { move(stops) };
        }
        case 1:
            return CubicBezier { adopt(easing.x1), adopt(easing.y1), adopt(easing.x2), adopt(easing.y2) };
        case 2:
            return Steps { adopt(easing.number_of_intervals), static_cast<StepPosition>(easing.step_position) };
        default:
            VERIFY_NOT_REACHED();
        }
    }())
{
}

EasingStyleValue::Function const& EasingStyleValue::function() const
{
    return m_function;
}

// https://drafts.csswg.org/css-easing/#linear-easing-function-serializing
void EasingStyleValue::Linear::serialize(Utf16StringBuilder& builder, SerializationMode mode) const
{
    // To serialize a linear() function:
    // 1. Let s be the string "linear(".
    builder.append("linear("_utf16);

    // 2. Serialize each control point of the function,
    // concatenate the results using the separator ", ",
    // and append the result to s.
    bool first = true;
    for (auto stop : stops) {
        if (first) {
            first = false;
        } else {
            builder.append(", "_utf16);
        }

        // To serialize a linear() control point:
        // 1. Let s be the serialization, as a <number>, of the control point's output progress value.
        stop.output->serialize(builder, mode);

        // 2. If the control point originally lacked an input progress value, return s.
        // 3. Otherwise, append " " (U+0020 SPACE) to s,
        // then serialize the control point's input progress value as a <percentage> and append it to s.
        if (stop.input) {
            builder.append_ascii(' ');
            stop.input->serialize(builder, mode);
        }

        // 4. Return s.
    }

    // 4. Append ")" to s, and return it.
    builder.append_ascii(')');
}

// https://drafts.csswg.org/css-easing/#bezier-serialization
void EasingStyleValue::CubicBezier::serialize(Utf16StringBuilder& builder, SerializationMode mode) const
{
    builder.append("cubic-bezier("_utf16);
    x1->serialize(builder, mode);
    builder.append(", "_utf16);
    y1->serialize(builder, mode);
    builder.append(", "_utf16);
    x2->serialize(builder, mode);
    builder.append(", "_utf16);
    y2->serialize(builder, mode);
    builder.append_ascii(')');
}

// https://drafts.csswg.org/css-easing/#steps-serialization
void EasingStyleValue::Steps::serialize(Utf16StringBuilder& builder, SerializationMode mode) const
{
    auto position = [&] -> Optional<StringView> {
        if (first_is_one_of(this->position, StepPosition::JumpEnd, StepPosition::End))
            return {};
        return CSS::to_string(this->position);
    }();

    builder.append("steps("_utf16);
    number_of_intervals->serialize(builder, mode);
    if (position.has_value()) {
        builder.append(", "_utf16);
        builder.append_ascii(position.value());
    }
    builder.append_ascii(')');
}

void EasingStyleValue::Function::serialize(StringBuilder& builder, SerializationMode mode) const
{
    Utf16StringBuilder utf16_builder;
    serialize(utf16_builder, mode);
    builder.append(utf16_builder.to_string().to_utf8());
}

void EasingStyleValue::Function::serialize(Utf16StringBuilder& builder, SerializationMode mode) const
{
    visit([&](auto const& curve) {
        curve.serialize(builder, mode);
    });
}

ValueComparingNonnullRefPtr<StyleValue const> EasingStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_function = function().visit(
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
