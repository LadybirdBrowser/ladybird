/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleSystemStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void CounterStyleSystemStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_value.visit(
        [&](CounterStyleSystem const& system) {
            builder.append(CSS::to_string(system));
        },
        [&](Fixed const& fixed) {
            builder.append("fixed"sv);
            if (fixed.first_symbol) {
                builder.append(' ');
                fixed.first_symbol->serialize(builder, mode);
            }
        },
        [&](Extends const& extends) {
            builder.append("extends "sv);
            serialize_an_identifier(builder, extends.name);
        });
}

ValueComparingNonnullRefPtr<StyleValue const> CounterStyleSystemStyleValue::absolutized(ComputationContext const& context) const
{
    return m_value.visit(
        [&](CounterStyleSystem const&) -> ValueComparingNonnullRefPtr<StyleValue const> {
            return *this;
        },
        [&](Fixed const& fixed) -> ValueComparingNonnullRefPtr<StyleValue const> {
            if (!fixed.first_symbol)
                return *this;

            auto const& absolutized_value = fixed.first_symbol->absolutized(context);

            if (absolutized_value == fixed.first_symbol)
                return *this;

            return CounterStyleSystemStyleValue::create_fixed(absolutized_value);
        },
        [&](Extends const&) -> ValueComparingNonnullRefPtr<StyleValue const> {
            return *this;
        });
}

bool CounterStyleSystemStyleValue::algorithm_differs_from(CounterStyleSystemStyleValue const& other) const
{
    if (m_value.index() != other.m_value.index())
        return true;

    return m_value.visit(
        [&](CounterStyleSystem const& system) -> bool {
            return system != other.m_value.get<CounterStyleSystem>();
        },
        [&](Fixed const&) -> bool {
            return false;
        },
        [&](Extends const& extends) -> bool {
            // FIXME: We don't know which counter style the 'extends' refers to here, so we have to assume it might
            //        differ if the names differ. Is this correct?
            return extends.name != other.m_value.get<Extends>().name;
        });
}

}
