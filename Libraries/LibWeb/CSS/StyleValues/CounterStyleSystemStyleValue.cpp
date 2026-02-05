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

bool CounterStyleSystemStyleValue::is_valid_symbol_count(size_t count) const
{
    return m_value.visit(
        [&](CounterStyleSystem const& system) -> bool {
            switch (system) {
            // https://drafts.csswg.org/css-counter-styles-3/#cyclic-system
            // If the system is cyclic, the symbols descriptor must contain at least one counter symbol, otherwise the
            // rule does not define a counter style (but is still a valid rule)
            case CounterStyleSystem::Cyclic:
            // https://drafts.csswg.org/css-counter-styles-3/#symbolic-system
            // If the system is symbolic, the symbols descriptor must contain at least one counter symbol, otherwise the
            // rule does not define a counter style (but is still a valid rule).
            case CounterStyleSystem::Symbolic:
                return count >= 1;
            // https://drafts.csswg.org/css-counter-styles-3/#alphabetic-system
            // If the system is alphabetic, the symbols descriptor must contain at least two counter symbols, otherwise
            // the rule does not define a counter style (but is still a valid rule).
            case CounterStyleSystem::Alphabetic:
            // https://drafts.csswg.org/css-counter-styles-3/#numeric-system
            // If the system is numeric, the symbols descriptor must contain at least two counter symbols, otherwise the
            // rule does not define a counter style (but is still a valid rule).
            case CounterStyleSystem::Numeric:
                return count >= 2;
            case CounterStyleSystem::Additive:
                // NB: Additive relies on the `additive-symbols` descriptor instead and `symbols` is ignored.
                return true;
            }

            VERIFY_NOT_REACHED();
        },
        [&](Fixed const&) -> bool {
            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            // If the system is fixed, the symbols descriptor must contain at least one counter symbol, otherwise the
            // rule does not define a counter style (but is still a valid rule).
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
            // https://drafts.csswg.org/css-counter-styles-3/#additive-system
            // If the system is additive, the additive-symbols descriptor must contain at least one additive tuple,
            // otherwise the rule does not define a counter style (but is still a valid rule).
            if (system == CounterStyleSystem::Additive)
                return count >= 1;

            // NB: Other systems ignore rely on the `symbols` descriptor instead and `additive-symbols` is ignored.
            return true;
        },
        [&](Fixed const&) -> bool {
            // NB: Fixed relies on the `symbols` descriptor instead and `additive-symbols` is ignored.
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
