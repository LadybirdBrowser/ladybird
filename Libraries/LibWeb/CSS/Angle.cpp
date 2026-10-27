/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>

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

}
