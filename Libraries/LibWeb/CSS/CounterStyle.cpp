/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyle.h"
#include <AK/HashTable.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-counter-styles-3/#decimal
CounterStyle CounterStyle::decimal()
{
    return CounterStyle::create(
        "decimal"_fly_string,
        GenericCounterStyleAlgorithm { CounterStyleSystem::Numeric, { "0"_fly_string, "1"_fly_string, "2"_fly_string, "3"_fly_string, "4"_fly_string, "5"_fly_string, "6"_fly_string, "7"_fly_string, "8"_fly_string, "9"_fly_string } },
        CounterStyleNegativeSign { .prefix = "-"_fly_string, .suffix = ""_fly_string },
        ""_fly_string,
        ". "_fly_string,
        { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } },
        {},
        CounterStylePad { .minimum_length = 0, .symbol = ""_fly_string });
}

// https://drafts.csswg.org/css-counter-styles-3/#disc
CounterStyle CounterStyle::disc()
{
    return CounterStyle::create(
        "disc"_fly_string,
        GenericCounterStyleAlgorithm { CounterStyleSystem::Cyclic, { "•"_fly_string } },
        CounterStyleNegativeSign { .prefix = ""_fly_string, .suffix = " "_fly_string },
        ""_fly_string,
        " "_fly_string,
        { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } },
        "decimal"_fly_string,
        CounterStylePad { .minimum_length = 0, .symbol = ""_fly_string });
}

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

Optional<String> CounterStyle::generate_an_initial_representation_for_the_counter_value(i64 value) const
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
        [&](FixedCounterStyleAlgorithm const& fixed_algorithm) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            // The first counter symbol is the representation for the first symbol value, and subsequent counter values
            // are represented by subsequent counter symbols. Once the list of counter symbols is exhausted, further
            // values cannot be represented by this counter style, and must instead be represented by the fallback
            // counter style.
            auto index = value - fixed_algorithm.first_symbol;

            if (index < 0 || index >= static_cast<i64>(fixed_algorithm.symbol_list.size()))
                return {};

            return fixed_algorithm.symbol_list[index].to_string();
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
            case CounterStyleSystem::Symbolic: {
                // https://drafts.csswg.org/css-counter-styles-3/#symbolic-system
                // To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S initially
                // be the empty string, and symbol(n) be the nth counter symbol in the list of counter symbols
                // (0-indexed).

                // 1. Let the chosen symbol be symbol( (value - 1) mod N).
                auto const& symbol = generic_algorithm.symbol_list[(value - 1) % generic_algorithm.symbol_list.size()];

                // 2. Let the representation length be ceil( value / N ).
                auto representation_length = (value + generic_algorithm.symbol_list.size() - 1) / generic_algorithm.symbol_list.size();

                // 3. Append the chosen symbol to S a number of times equal to the representation length.
                // Finally, return S.
                return MUST(String::repeated(symbol.to_string(), representation_length));
            }
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-negative
bool CounterStyle::uses_a_negative_sign() const
{
    // Not all system values use a negative sign. In particular, a counter style uses a negative sign if its system
    // value is symbolic, alphabetic, numeric, additive, or extends if the extended counter style itself uses a negative
    // sign.
    // NB: We have resolved extends to the underlying algorithm before calling this
    return m_algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const&) -> bool {
            return true;
        },
        [&](FixedCounterStyleAlgorithm const&) -> bool {
            return false;
        },
        [&](GenericCounterStyleAlgorithm const& generic_system) -> bool {
            switch (generic_system.type) {
            case CounterStyleSystem::Cyclic:
                return false;
            case CounterStyleSystem::Symbolic:
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Numeric:
                return true;
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#generate-a-counter
static String generate_a_counter_representation_impl(Optional<CounterStyle const&> const& counter_style, HashMap<FlyString, CounterStyle> const& registered_counter_styles, i32 value, HashTable<FlyString>& fallback_history)
{
    // When asked to generate a counter representation using a particular counter style for a particular
    // counter value, follow these steps:

    // 1. If the counter style is unknown, exit this algorithm and instead generate a counter representation using the
    //    decimal style and the same counter value.
    if (!counter_style.has_value())
        return generate_a_counter_representation_impl(CounterStyle::decimal(), registered_counter_styles, value, fallback_history);

    auto const generate_a_counter_representation_using_fallback = [&]() {
        VERIFY(counter_style->name() != "decimal"_fly_string);

        auto const& fallback_name = counter_style->fallback().value();
        auto const& fallback = registered_counter_styles.get(fallback_name);

        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-fallback
        // If the value of the fallback descriptor isn’t the name of any defined counter style, the used value of the
        // fallback descriptor is decimal instead. Similarly, while following fallbacks to find a counter style that
        // can render the given counter value, if a loop in the specified fallbacks is detected, the decimal style must
        // be used instead.
        if (!fallback.has_value() || fallback_history.contains(fallback_name))
            return generate_a_counter_representation_impl(CounterStyle::decimal(), registered_counter_styles, value, fallback_history);

        fallback_history.set(counter_style->name());

        return generate_a_counter_representation_impl(fallback, registered_counter_styles, value, fallback_history);
    };

    // 2. If the counter value is outside the range of the counter style, exit this algorithm and instead generate a
    //    counter representation using the counter style’s fallback style and the same counter value.
    if (!any_of(counter_style->range(), [&](auto const& entry) { return value >= entry.start && value <= entry.end; }))
        return generate_a_counter_representation_using_fallback();

    auto value_is_negative_and_uses_negative_sign = value < 0 && counter_style->uses_a_negative_sign();

    // 3. Using the counter value and the counter algorithm for the counter style, generate an initial representation
    //    for the counter value. If the counter value is negative and the counter style uses a negative sign, instead
    //    generate an initial representation using the absolute value of the counter value.
    auto maybe_representation = counter_style->generate_an_initial_representation_for_the_counter_value(value_is_negative_and_uses_negative_sign ? abs(static_cast<i64>(value)) : static_cast<i64>(value));

    // AD-HOC: Algorithms are sometimes unable to produce a representation and require us to use the fallback - we
    //         represent this by returning an empty Optional.
    if (!maybe_representation.has_value())
        return generate_a_counter_representation_using_fallback();

    auto representation = maybe_representation.value();

    // 4. Prepend symbols to the representation as specified in the pad descriptor.
    {
        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-pad
        // Let difference be the provided <integer> minus the number of grapheme clusters in the initial representation
        // for the counter value.
        // FIXME: We should be counting grapheme clusters here.
        auto difference = counter_style->pad().minimum_length - static_cast<i32>(representation.length_in_code_units());

        // If the counter value is negative and the counter style uses a negative sign, further reduce difference by
        // the number of grapheme clusters in the counter style’s negative descriptor’s <symbol>(s).
        // FIXME: We should be counting grapheme clusters here.
        if (value_is_negative_and_uses_negative_sign)
            difference -= counter_style->negative_sign().prefix.to_string().length_in_code_units() + counter_style->negative_sign().suffix.to_string().length_in_code_units();

        // If difference is greater than zero, prepend difference copies of the specified <symbol> to the representation.
        if (difference > 0)
            representation = MUST(String::formatted("{}{}", MUST(String::repeated(counter_style->pad().symbol.to_string(), difference)), representation));
    }

    // 5. If the counter value is negative and the counter style uses a negative sign, wrap the representation in the
    //    counter style’s negative sign as specified in the negative descriptor.
    if (value_is_negative_and_uses_negative_sign)
        representation = MUST(String::formatted("{}{}{}", counter_style->negative_sign().prefix, representation, counter_style->negative_sign().suffix));

    // 6. Return the representation.
    return representation;
}

String generate_a_counter_representation(Optional<CounterStyle const&> const& counter_style, HashMap<FlyString, CounterStyle> const& registered_counter_styles, i32 value)
{
    HashTable<FlyString> fallback_history;
    return generate_a_counter_representation_impl(counter_style, registered_counter_styles, value, fallback_history);
}

}
