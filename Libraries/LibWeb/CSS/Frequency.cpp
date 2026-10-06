/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

Frequency::Frequency(double value, FrequencyUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

double Frequency::to_hertz() const
{
    return ratio_between_units(m_unit, FrequencyUnit::Hz) * m_value;
}

}
