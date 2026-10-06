/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>

namespace Web::CSS {

Flex::Flex(double value, FlexUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

double Flex::to_fr() const
{
    return ratio_between_units(m_unit, FlexUnit::Fr) * m_value;
}

}
