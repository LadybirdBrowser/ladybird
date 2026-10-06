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

namespace Web::CSS {

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
            auto canonicalized = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_linear_easing_canonicalize(style_value.rust_style_value_data()));
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

double EasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    Vector<Compositing::RustFFI::FfiLinearEasingPoint> linear_points;
    auto descriptor = to_ffi_easing_descriptor<Compositing::RustFFI::FfiEasingDescriptor>(*this, linear_points);
    return StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

Utf16String const& EasingFunction::to_utf16_string() const
{
    return visit(
        [](auto const& function) -> Utf16String const& {
            return function.stringified;
        });
}

}
