/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>

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

Resolution Resolution::from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (style_value->is_resolution())
        return style_value->as_resolution().resolution();

    if (style_value->is_calculated())
        return style_value->as_calculated().resolve_resolution({}).value();

    VERIFY_NOT_REACHED();
}

double Resolution::to_dots_per_pixel() const
{
    return ratio_between_units(m_unit, ResolutionUnit::Dppx) * m_value;
}

}
