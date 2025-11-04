/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RandomValueSharingStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> RandomValueSharingStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (m_fixed_value) {
        auto const& absolutized_fixed_value = m_fixed_value->absolutized(computation_context);

        if (m_fixed_value == absolutized_fixed_value)
            return *this;

        return RandomValueSharingStyleValue::create_fixed(absolutized_fixed_value);
    }

    TODO();
}

double RandomValueSharingStyleValue::random_base_value() const
{
    VERIFY(m_fixed_value);
    VERIFY(m_fixed_value->is_number() || (m_fixed_value->is_calculated() && m_fixed_value->as_calculated().resolves_to_number()));

    if (m_fixed_value->is_number())
        return m_fixed_value->as_number().number();

    if (m_fixed_value->is_calculated())
        return m_fixed_value->as_calculated().resolve_number({}).value();

    VERIFY_NOT_REACHED();
}

String RandomValueSharingStyleValue::to_string(SerializationMode serialization_mode) const
{
    if (m_fixed_value)
        return MUST(String::formatted("fixed {}", m_fixed_value->to_string(serialization_mode)));

    TODO();
}

}
