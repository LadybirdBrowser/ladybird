/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyle.h"
#include <AK/HashTable.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

CounterStyle CounterStyle::from_counter_style_definition(CounterStyleDefinition const& definition, HashMap<FlyString, CounterStyle> const& registered_counter_styles)
{
    return definition.algorithm().visit(
        [&](CounterStyleSystemStyleValue::Extends const& extends) {
            // NB: The caller should ensure that this is always set (i.e. by ensuring the relevant rule is registered
            //     before this one, and replacing the extended counter style with "decimal" if it is not defined).
            auto extended_counter_style = registered_counter_styles.get(extends.name).value();

            return CounterStyle::create(
                definition.name(),
                extended_counter_style.algorithm(),
                definition.negative_sign().value_or(extended_counter_style.negative_sign()),
                definition.prefix().value_or(extended_counter_style.prefix()),
                definition.suffix().value_or(extended_counter_style.suffix()),
                definition.range().visit(
                    [&](Empty const&) { return extended_counter_style.range(); },
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](AutoRange const&) { return AutoRange::resolve(extended_counter_style.algorithm()); }),
                definition.fallback().value_or(extended_counter_style.fallback().value_or("decimal"_fly_string)),
                definition.pad().value_or(extended_counter_style.pad()));
        },
        [&](CounterStyleAlgorithm const& algorithm) {
            return CounterStyle::create(
                definition.name(),
                algorithm,
                definition.negative_sign().value_or({ .prefix = "-"_fly_string, .suffix = ""_fly_string }),
                definition.prefix().value_or(""_fly_string), definition.suffix().value_or(". "_fly_string),
                definition.range().visit(
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](auto const&) { return AutoRange::resolve(algorithm); }),
                definition.fallback().value_or("decimal"_fly_string),
                definition.pad().value_or({ .minimum_length = 0, .symbol = ""_fly_string }));
        });
}

Optional<String> CounterStyle::generate_an_initial_representation_for_the_counter_value(i32) const
{
    return m_algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const&) -> Optional<String> {
            TODO();
        },
        [&](FixedCounterStyleAlgorithm const&) -> Optional<String> {
            TODO();
        },
        [&](GenericCounterStyleAlgorithm const& generic_algorithm) -> Optional<String> {
            switch (generic_algorithm.type) {
            case CounterStyleSystem::Cyclic:
            case CounterStyleSystem::Numeric:
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Symbolic:
                TODO();
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        });
}

}
