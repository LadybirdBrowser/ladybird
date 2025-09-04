/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

Angle::Angle(double value, AngleUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Angle Angle::make_degrees(double value)
{
    return { value, AngleUnit::Deg };
}

Angle Angle::percentage_of(Percentage const& percentage) const
{
    return Angle { percentage.as_fraction() * m_value, m_unit };
}

String Angle::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <angle>
    // The <number> component serialized as per <number> followed by the unit in canonical form as defined in its
    // respective specification.
    if (serialization_mode == SerializationMode::ResolvedValue) {
        StringBuilder builder;
        serialize_a_number(builder, to_degrees());
        builder.append("deg"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, raw_value());
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

double Angle::to_degrees() const
{
    return ratio_between_units(m_unit, AngleUnit::Deg) * m_value;
}

double Angle::to_radians() const
{
    return ratio_between_units(m_unit, AngleUnit::Rad) * m_value;
}

Angle Angle::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, Angle const& reference_value)
{
    CalculationResolutionContext context {
        .percentage_basis = reference_value,
        .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node),
    };
    return calculated->resolve_angle(context).value();
}

}
