/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Percentage.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Percentage Percentage::from_style_value(NonnullRefPtr<StyleValue const> const& value)
{
    if (value->is_percentage())
        return value->as_percentage().percentage();

    if (value->is_calculated())
        return value->as_calculated().resolve_percentage({}).value();

    VERIFY_NOT_REACHED();
}

}
