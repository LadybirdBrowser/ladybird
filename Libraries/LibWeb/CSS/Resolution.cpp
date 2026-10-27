/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

Resolution::Resolution(double value, ResolutionUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}

Resolution Resolution::make_dots_per_pixel(double value)
{
    return { value, ResolutionUnit::Dppx };
}

double Resolution::to_dots_per_pixel() const
{
    return ratio_between_units(m_unit, ResolutionUnit::Dppx) * m_value;
}

}
