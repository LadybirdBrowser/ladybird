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
            builder.appendff("extends {}", serialize_an_identifier(extends.name));
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

bool CounterStyleSystemStyleValue::is_valid_symbol_count(size_t count) const
{
    return m_value.visit(
        [&](CounterStyleSystem const& system) -> bool {
            switch (system) {
            case CounterStyleSystem::Cyclic:
            case CounterStyleSystem::Symbolic:
                return count >= 1;
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Numeric:
                return count >= 2;
            case CounterStyleSystem::Additive:
                // NB: Additive relies on the `additive-symbols` descriptor instead
                return true;
            }

            VERIFY_NOT_REACHED();
        },
        [&](Fixed const&) -> bool {
            return count >= 1;
        },
        [&](Extends const&) -> bool {
            // https://drafts.csswg.org/css-counter-styles-3/#extends-system
            // If a @counter-style uses the extends system, it must not contain a symbols or additive-symbols
            // descriptor, otherwise the rule does not define a counter style (but is still a valid rule).
            return false;
        });
}

bool CounterStyleSystemStyleValue::is_valid_additive_symbol_count(size_t count) const
{
    return m_value.visit(
        [&](CounterStyleSystem const& system) -> bool {
            if (system == CounterStyleSystem::Additive)
                return count >= 1;

            return true;
        },
        [&](Fixed const&) -> bool {
            return true;
        },
        [&](Extends const&) -> bool {
            // https://drafts.csswg.org/css-counter-styles-3/#extends-system
            // If a @counter-style uses the extends system, it must not contain a symbols or additive-symbols
            // descriptor, otherwise the rule does not define a counter style (but is still a valid rule).
            return false;
        });
}

}
