/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuperellipseStyleValue.h"
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

String SuperellipseStyleValue::to_string(SerializationMode mode) const
{
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

}
