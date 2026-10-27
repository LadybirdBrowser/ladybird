/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Length Length::percentage_of(Percentage const& percentage) const
{
    return Length { percentage.as_fraction() * raw_value(), m_unit };
}

void Length::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <length>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.

    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_unit != LengthUnit::Px) {
        serialize_a_number(builder, absolute_length_to_px().to_double());
        builder.append("px"sv);
        return;
    }
    serialize_a_number(builder, m_value);
    builder.append(unit_name());
}

void Length::serialize(Utf16StringBuilder& builder, SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <length>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.

    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_unit != LengthUnit::Px) {
        serialize_a_number(builder, absolute_length_to_px().to_double());
        builder.append_ascii("px"sv);
        return;
    }
    serialize_a_number(builder, m_value);
    builder.append(unit_name());
}

Length Length::from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<Length> percentage_basis)
{
    if (style_value->is_length())
        return style_value->as_length().length();

    if (style_value->is_calculated()) {
        CalculationResolutionContext::PercentageBasis resolved_percentage_basis;

        if (percentage_basis.has_value()) {
            resolved_percentage_basis = percentage_basis.value();
        }

        return style_value->as_calculated().resolve_length({ .percentage_basis = resolved_percentage_basis }).value();
    }

    if (style_value->is_percentage()) {
        VERIFY(percentage_basis.has_value());

        return percentage_basis.value().percentage_of(style_value->as_percentage().percentage());
    }

    VERIFY_NOT_REACHED();
}

}
