/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Time.h"
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

Time::Time(double value, TimeUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Time Time::make_seconds(double value)
{
    return { value, TimeUnit::S };
}

double Time::to_seconds() const
{
    return ratio_between_units(m_unit, TimeUnit::S) * m_value;
}

double Time::to_milliseconds() const
{
    return ratio_between_units(m_unit, TimeUnit::Ms) * m_value;
}

}
