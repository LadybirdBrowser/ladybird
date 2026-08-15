/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuperellipseStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> SuperellipseStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_parameter = parameter_style_value()->absolutized(computation_context);

    if (absolutized_parameter == parameter_style_value())
        return *this;

    return SuperellipseStyleValue::create(absolutized_parameter);
}

}
