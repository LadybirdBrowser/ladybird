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

Optional<String> CounterStyle::generate_an_initial_representation_for_the_counter_value(i32 value) const
{
    return m_algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const& additive_algorithm) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#additive-system
            // To construct the representation:

            // 1. Let value initially be the counter value, S initially be the empty string, and symbol list initially
            //    be the list of additive tuples.

            // 2. If value is zero:
            if (value == 0) {
                // 1. If symbol list contains a tuple with a weight of zero, append that tuple’s counter symbol to S and
                //    return S.
                if (auto it = additive_algorithm.symbol_list.find_if([](auto const& tuple) { return tuple.weight == 0; }); it != additive_algorithm.symbol_list.end())
                    return it->symbol.to_string();

                // 2. Otherwise, the given counter value cannot be represented by this counter style, and must instead
                //    be represented by the fallback counter style.
                return {};
            }

            StringBuilder builder;

            // 3. For each tuple in symbol list:
            for (auto const& tuple : additive_algorithm.symbol_list) {
                // 1. Let symbol and weight be tuple’s counter symbol and weight, respectively.

                // 2. If weight is zero, or weight is greater than value, continue.
                if (tuple.weight == 0 || tuple.weight > value)
                    continue;

                // 3. Let reps be floor( value / weight ).
                auto reps = value / tuple.weight;

                // 4. Append symbol to S reps times.
                for (int i = 0; i < reps; ++i)
                    builder.append(tuple.symbol);

                // 5. Decrement value by weight * reps.
                value -= tuple.weight * reps;

                // 6. If value is zero, return S.
                if (value == 0)
                    return MUST(builder.to_string());
            }

            // Assertion: value is still non-zero.
            VERIFY(value != 0);

            // The given counter value cannot be represented by this counter style, and must instead be represented by
            // the fallback counter style.
            return {};
        },
        [&](FixedCounterStyleAlgorithm const&) -> Optional<String> {
            TODO();
        },
        [&](GenericCounterStyleAlgorithm const& generic_algorithm) -> Optional<String> {
            switch (generic_algorithm.type) {
            case CounterStyleSystem::Cyclic: {
                // https://drafts.csswg.org/css-counter-styles-3/#cyclic-system
                // If there are N counter symbols and a representation is being constructed for the integer value, the
                // representation is the counter symbol at index ( (value-1) mod N) of the list of counter symbols
                // (0-indexed).
                return generic_algorithm.symbol_list[(value - 1) % generic_algorithm.symbol_list.size()].to_string();
            }
            case CounterStyleSystem::Numeric: {
                // https://drafts.csswg.org/css-counter-styles-3/#numeric-system
                // If there are N counter symbols, the representation is a base N number using the counter symbols as
                // digits. To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S
                // initially be the empty string, and symbol(n) be the nth counter symbol in the list of counter
                // symbols (0-indexed).

                // 1. If value is 0, append symbol(0) to S and return S.
                if (value == 0)
                    return generic_algorithm.symbol_list[0].to_string();

                // NB: Our string builder doesn't support prepending, so we use a vector and convert that to a string at
                //     the end.
                Vector<CounterStyleSymbol> symbols;

                // 2. While value is not equal to 0:
                while (value != 0) {
                    // 1. Prepend symbol( value mod N ) to S.
                    symbols.prepend(generic_algorithm.symbol_list[value % generic_algorithm.symbol_list.size()]);

                    // 2. Set value to floor( value / N ).
                    value /= generic_algorithm.symbol_list.size();
                }

                // 3. Return S.
                return MUST(String::join(""sv, symbols));
            }
            case CounterStyleSystem::Alphabetic: {
                // https://drafts.csswg.org/css-counter-styles-3/#alphabetic-system
                // If there are N counter symbols, the representation is a base N alphabetic number using the counter
                // symbols as digits. To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S initially
                // be the empty string, and symbol(n) be the nth counter symbol in the list of counter symbols
                // (0-indexed).

                // NB: Our string builder doesn't support prepending, so we use a vector and convert that to a string at
                //     the end.
                Vector<CounterStyleSymbol> symbols;

                // While value is not equal to 0:
                while (value != 0) {
                    // 1. Set value to value - 1.
                    value -= 1;

                    // 2. Prepend symbol( value mod N ) to S.
                    symbols.prepend(generic_algorithm.symbol_list[value % generic_algorithm.symbol_list.size()]);

                    // 3. Set value to floor( value / N ).
                    value /= generic_algorithm.symbol_list.size();
                }

                // Finally, return S.
                return MUST(String::join(""sv, symbols));
            }
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
