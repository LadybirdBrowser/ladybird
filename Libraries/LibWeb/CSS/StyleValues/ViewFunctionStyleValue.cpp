/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ViewFunctionStyleValue.h"

namespace Web::CSS {

String ViewFunctionStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("view("sv);

    if (m_axis != Axis::Block)
        builder.append(CSS::to_string(m_axis));

    auto stringified_inset = m_inset->to_string(mode);

    if (stringified_inset != "auto"sv) {
        if (m_axis != Axis::Block)
            builder.append(' ');

        builder.append(stringified_inset);
    }

    builder.append(')');
    return builder.to_string_without_validation();
}

ValueComparingNonnullRefPtr<StyleValue const> ViewFunctionStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_inset = m_inset->absolutized(computation_context);

    if (absolutized_inset == m_inset)
        return *this;

    return ViewFunctionStyleValue::create(m_axis, absolutized_inset);
}

}
