/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EasingFunction.h"
#include <AK/Math.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>

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
        auto const& easing = style_value.rust_style_value_data()->easing;
        auto numeric = [](auto const& retained) -> double {
            auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(retained.pointer);
            switch (data->tag) {
            case StyleValueFFI::StyleValueData::Tag::Number:
                return data->number.value;
            case StyleValueFFI::StyleValueData::Tag::Integer:
                return data->integer.value;
            case StyleValueFFI::StyleValueData::Tag::Percentage:
                return data->percentage.value;
            case StyleValueFFI::StyleValueData::Tag::Calculated: {
                auto calculated = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data));
                auto const& value = calculated->as_calculated();
                if (value.resolves_to_percentage())
                    return value.resolve_percentage({}).value().value();
                return value.resolve_number({}).value();
            }
            default:
                VERIFY_NOT_REACHED();
            }
        };
        switch (easing.kind) {
        case 0: {
            auto canonicalized = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_composite_style_value_absolutize(
                style_value.rust_style_value_data(), nullptr, [](void const*, StyleValueFFI::StyleValueData const* child) {
                    return StyleValueFFI::rust_style_value_retain(child);
                }));
            auto const& canonical_easing = canonicalized->rust_style_value_data()->easing;
            Vector<LinearEasingFunction::ControlPoint> points;
            points.ensure_capacity(canonical_easing.linear_stops.length);
            for (auto const& stop : ReadonlySpan<StyleValueFFI::RetainedLinearEasingStop> { canonical_easing.linear_stops.pointer, canonical_easing.linear_stops.length })
                points.unchecked_append({ numeric(stop.input) / 100, numeric(stop.output) });
            return LinearEasingFunction { move(points), style_value.to_utf16_string(SerializationMode::ResolvedValue) };
        }
        case 1:
            return CubicBezierEasingFunction {
                numeric(easing.x1),
                numeric(easing.y1),
                numeric(easing.x2),
                numeric(easing.y2),
                style_value.to_utf16_string(SerializationMode::Normal),
            };
        case 2:
            return StepsEasingFunction { round_to_nearest_integer(numeric(easing.number_of_intervals)), static_cast<StepPosition>(easing.step_position), style_value.to_utf16_string(SerializationMode::ResolvedValue) };
        default:
            VERIFY_NOT_REACHED();
        }
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

    StyleValueFFI::FfiEasingDescriptor descriptor {};
    Vector<StyleValueFFI::FfiLinearEasingPoint> points;
    visit(
        [&](LinearEasingFunction const& linear) {
            descriptor.kind = StyleValueFFI::FfiEasingKind::Linear;
            points.ensure_capacity(linear.control_points.size());
            for (auto const& point : linear.control_points)
                points.unchecked_append({ point.input, point.output });
            descriptor.linear_points = points.data();
            descriptor.linear_point_count = points.size();
        },
        [&](CubicBezierEasingFunction const& bezier) {
            descriptor.kind = StyleValueFFI::FfiEasingKind::CubicBezier;
            descriptor.x1 = bezier.x1;
            descriptor.y1 = bezier.y1;
            descriptor.x2 = bezier.x2;
            descriptor.y2 = bezier.y2;
        },
        [&](StepsEasingFunction const& steps) {
            descriptor.kind = StyleValueFFI::FfiEasingKind::Steps;
            descriptor.interval_count = steps.interval_count;
            descriptor.step_position = to_underlying(steps.position);
        });
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_from_easing(&descriptor));
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
