/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontStyleStyleValue.h"
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

FontStyleStyleValue::FontStyleStyleValue(FontStyleKeyword font_style, ValueComparingRefPtr<StyleValue const> angle_value, ValueComparingRefPtr<StyleValue const> second_angle_value)
    : StyleValueWithDefaultOperators(Type::FontStyle)
    , m_font_style(font_style)
    , m_angle_value(angle_value)
    , m_second_angle_value(second_angle_value)
{
}

FontStyleStyleValue::~FontStyleStyleValue() = default;

int FontStyleStyleValue::to_font_slope() const
{
    // FIXME: Implement `left`, `right`, and `oblique <angle>`
    switch (as_font_style().font_style()) {
    case FontStyleKeyword::Italic:
        static int italic_slope = Gfx::name_to_slope("Italic"sv);
        return italic_slope;
    case FontStyleKeyword::Oblique:
        static int oblique_slope = Gfx::name_to_slope("Oblique"sv);
        return oblique_slope;
    case FontStyleKeyword::Normal:
    default:
        static int normal_slope = Gfx::name_to_slope("Normal"sv);
        return normal_slope;
    }
}

void FontStyleStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (m_angle_value) {
        Optional<String> angle_string;
        bool is_zero_angle = false;

        if (m_angle_value->is_angle()) {
            auto angle = m_angle_value->as_angle().angle();
            angle_string = angle.to_string(mode);
            is_zero_angle = (angle.to_degrees() == 0.0);

            if (m_second_angle_value && m_second_angle_value->is_angle()) {
                auto second_angle = m_second_angle_value->as_angle().angle();
                // https://drafts.csswg.org/css-fonts-4/#serializing-at-rules
                // "if the start and end values are the same (the range is zero)
                //  the descriptor is serialized as a single value, not a range."
                // FIXME: The WPT test at-font-face-descriptors.html is missing an
                // expectedValue for "oblique 20deg 20deg" (defaults to the input
                // string). Per §13.2 (added by w3c/csswg-drafts#7964), equal bounds
                // should serialize as a single value, so we correctly produce
                // "oblique 20deg". This spec-correct behavior causes a test regression.
                if (angle.to_degrees() != second_angle.to_degrees()) {
                    builder.append(CSS::to_string(m_font_style));
                    builder.appendff(" {}", *angle_string);
                    builder.appendff(" {}", m_second_angle_value->to_string(mode));
                    return;
                }
                // Equal bounds collapsed — fall through to single-angle handling
            } else if (m_second_angle_value) {
                // Second angle is not an AngleStyleValue (e.g. calc), can't collapse
                builder.append(CSS::to_string(m_font_style));
                builder.appendff(" {}", *angle_string);
                builder.appendff(" {}", m_second_angle_value->to_string(mode));
                return;
            }
        } else {
            angle_string = m_angle_value->to_string(mode);
            if (m_second_angle_value) {
                builder.append(CSS::to_string(m_font_style));
                builder.appendff(" {}", *angle_string);
                builder.appendff(" {}", m_second_angle_value->to_string(mode));
                return;
            }
        }

        // https://drafts.csswg.org/css-fonts/#valdef-font-style-normal
        // normal "represents an oblique value of 0"
        if (m_font_style == FontStyleKeyword::Oblique && is_zero_angle) {
            builder.append("normal"sv);
            return;
        }

        builder.append(CSS::to_string(m_font_style));
        // https://drafts.csswg.org/css-fonts/#valdef-font-style-oblique-angle--90deg-90deg
        // The lack of an <angle> represents 14deg.
        if (angle_string.has_value() && *angle_string != "14deg"sv)
            builder.appendff(" {}", *angle_string);
        return;
    }

    builder.append(CSS::to_string(m_font_style));
}

ValueComparingNonnullRefPtr<StyleValue const> FontStyleStyleValue::absolutized(ComputationContext const& computation_context) const
{
    ValueComparingRefPtr<StyleValue const> absolutized_angle;
    ValueComparingRefPtr<StyleValue const> absolutized_second_angle;

    if (m_angle_value)
        absolutized_angle = m_angle_value->absolutized(computation_context);
    if (m_second_angle_value)
        absolutized_second_angle = m_second_angle_value->absolutized(computation_context);

    // If the inner angle is still a CalculatedStyleValue (e.g. calc(10deg - 10deg)),
    // resolve it to a concrete AngleStyleValue so serialization can use numeric
    // comparison for the zero-angle check and equal-bound collapsing.
    if (absolutized_angle && absolutized_angle->is_calculated()) {
        auto resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
        if (auto resolved = absolutized_angle->as_calculated().resolve_angle(resolution_context); resolved.has_value())
            absolutized_angle = AngleStyleValue::create(resolved.release_value());
    }
    if (absolutized_second_angle && absolutized_second_angle->is_calculated()) {
        auto resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
        if (auto resolved = absolutized_second_angle->as_calculated().resolve_angle(resolution_context); resolved.has_value())
            absolutized_second_angle = AngleStyleValue::create(resolved.release_value());
    }

    if (absolutized_angle == m_angle_value && absolutized_second_angle == m_second_angle_value)
        return *this;

    return FontStyleStyleValue::create(m_font_style, absolutized_angle, absolutized_second_angle);
}

}
