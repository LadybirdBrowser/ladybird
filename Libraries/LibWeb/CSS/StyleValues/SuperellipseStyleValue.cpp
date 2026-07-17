/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuperellipseStyleValue.h"

namespace Web::CSS {

void SuperellipseStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (mode == SerializationMode::ResolvedValue && parameter_style_value()->is_number()) {
        auto number = parameter_style_value()->as_number().number();

        if (number == 1) {
            builder.append("round"sv);
            return;
        }

        if (number == 2) {
            builder.append("squircle"sv);
            return;
        }

        if (number == AK::Infinity<double>) {
            builder.append("square"sv);
            return;
        }

        if (number == 0) {
            builder.append("bevel"sv);
            return;
        }

        if (number == -1) {
            builder.append("scoop"sv);
            return;
        }

        if (number == -AK::Infinity<double>) {
            builder.append("notch"sv);
            return;
        }
    }

    builder.append("superellipse("sv);
    if (!parameter_style_value()->is_number()) {
        parameter_style_value()->serialize(builder, mode);
    } else {
        auto number = parameter_style_value()->as_number().number();
        if (number == AK::Infinity<double>) {
            builder.append("infinity"sv);
        } else if (number == -AK::Infinity<double>) {
            builder.append("-infinity"sv);
        } else {
            parameter_style_value()->serialize(builder, mode);
        }
    }
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> SuperellipseStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_parameter = parameter_style_value()->absolutized(computation_context);

    if (absolutized_parameter == parameter_style_value())
        return *this;

    return SuperellipseStyleValue::create(absolutized_parameter);
}

}
