/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuperellipseStyleValue.h"

namespace Web::CSS {

void SuperellipseStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (mode == SerializationMode::ResolvedValue && m_parameter->is_number()) {
        auto number = m_parameter->as_number().number();

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
    if (!m_parameter->is_number()) {
        m_parameter->serialize(builder, mode);
    } else {
        auto number = m_parameter->as_number().number();
        if (number == AK::Infinity<double>) {
            builder.append("infinity"sv);
        } else if (number == -AK::Infinity<double>) {
            builder.append("-infinity"sv);
        } else {
            m_parameter->serialize(builder, mode);
        }
    }
    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> SuperellipseStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_parameter = m_parameter->absolutized(computation_context);

    if (absolutized_parameter == m_parameter)
        return *this;

    return SuperellipseStyleValue::create(absolutized_parameter);
}

}
