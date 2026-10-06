/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

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

double Angle::to_degrees() const
{
    return ratio_between_units(m_unit, AngleUnit::Deg) * m_value;
}

double Angle::to_radians() const
{
    return ratio_between_units(m_unit, AngleUnit::Rad) * m_value;
}

Angle Angle::from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<Angle> percentage_basis)
{
    if (style_value->is_angle())
        return style_value->as_angle().angle();

    if (style_value->is_calculated()) {
        CalculationResolutionContext::PercentageBasis resolved_percentage_basis;

        if (percentage_basis.has_value()) {
            resolved_percentage_basis = percentage_basis.value();
        }

        return style_value->as_calculated().resolve_angle({ .percentage_basis = resolved_percentage_basis }).value();
    }

    if (style_value->is_percentage()) {
        VERIFY(percentage_basis.has_value());

        return percentage_basis.value().percentage_of(style_value->as_percentage().percentage());
    }

    VERIFY_NOT_REACHED();
}

}
