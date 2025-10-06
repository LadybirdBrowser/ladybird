/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuperellipseStyleValue.h"

namespace Web::CSS {

String SuperellipseStyleValue::to_string(SerializationMode mode) const
{
    if (mode == SerializationMode::ResolvedValue && m_parameter->is_number()) {
        auto number = m_parameter->as_number().number();

        if (number == 1)
            return "round"_string;

        if (number == 2)
            return "squircle"_string;

        if (number == AK::Infinity<double>)
            return "square"_string;

        if (number == 0)
            return "bevel"_string;

        if (number == -1)
            return "scoop"_string;

        if (number == -AK::Infinity<double>)
            return "notch"_string;
    }

    auto stringified_parameter = [&] {
        if (!m_parameter->is_number())
            return m_parameter->to_string(mode);

        auto number = m_parameter->as_number().number();

        if (number == AK::Infinity<double>)
            return "infinity"_string;

        if (number == -AK::Infinity<double>)
            return "-infinity"_string;

        return m_parameter->to_string(mode);
    }();

    return MUST(String::formatted("superellipse({})", stringified_parameter));
}

ValueComparingNonnullRefPtr<StyleValue const> SuperellipseStyleValue::absolutized(ComputationContext const& computation_context, PropertyComputationDependencies& property_computation_dependencies) const
{
    auto const& absolutized_parameter = m_parameter->absolutized(computation_context, property_computation_dependencies);

    if (absolutized_parameter == m_parameter)
        return *this;

    return SuperellipseStyleValue::create(absolutized_parameter);
}

}
