/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TupleStyleValue.h"

namespace Web::CSS {

void TupleStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto first = true;

    for (auto const& value : m_tuple) {
        if (value) {
            if (!first)
                builder.append(' ');
            value->serialize(builder, mode);
            first = false;
        }
    }
}

ValueComparingNonnullRefPtr<StyleValue const> TupleStyleValue::absolutized(ComputationContext const& context) const
{
    StyleValueTuple absolutized_tuple;
    absolutized_tuple.ensure_capacity(m_tuple.size());

    bool any_value_changed = false;

    for (auto const& value : m_tuple) {
        if (value) {
            auto absolutized_value = value->absolutized(context);

            if (absolutized_value != value)
                any_value_changed = true;

            absolutized_tuple.append(move(absolutized_value));
        } else {
            absolutized_tuple.append(nullptr);
        }
    }

    if (!any_value_changed)
        return *this;

    return TupleStyleValue::create(move(absolutized_tuple));
}

}
