/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleDefinition.h"
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-counter-styles-3/#valdef-counter-style-range-auto
Vector<CounterStyleRangeEntry> AutoRange::resolve(CounterStyleAlgorithm const& algorithm)
{
    // The range depends on the counter system:
    return algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const&) -> Vector<CounterStyleRangeEntry> {
            // For additive systems, the range is 0 to positive infinity.
            return { { 0, NumericLimits<i64>::max() } };
        },
        [&](FixedCounterStyleAlgorithm const&) -> Vector<CounterStyleRangeEntry> {
            // For cyclic, numeric, and fixed systems, the range is negative infinity to positive infinity.
            // NB: cyclic and numeric are handled below.
            return { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } };
        },
        [&](GenericCounterStyleAlgorithm const& generic_algorithm) -> Vector<CounterStyleRangeEntry> {
            switch (generic_algorithm.type) {
            case CounterStyleSystem::Cyclic:
            case CounterStyleSystem::Numeric:
                // For cyclic, numeric, and fixed systems, the range is negative infinity to positive infinity.
                // NB: Fixed is handled above.
                return { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } };
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Symbolic:
                // For alphabetic and symbolic systems, the range is 1 to positive infinity.
                return { { 1, NumericLimits<i64>::max() } };
            case CounterStyleSystem::Additive:
                // NB: Additive is handled above.
                VERIFY_NOT_REACHED();
            }
            VERIFY_NOT_REACHED();
        });
}

Optional<CounterStyleDefinition> CounterStyleDefinition::from_counter_style_rule(CSSCounterStyleRule const& rule, ComputationContext const& computation_context)
{
    if (!rule.system_style_value())
        return {};

    auto maybe_algorithm = resolve_algorithm(*rule.system_style_value(), rule.symbols_style_value(), rule.additive_symbols_style_value(), computation_context);

    if (maybe_algorithm.has<Empty>())
        return {};

    return CounterStyleDefinition::create(
        rule.name(),
        maybe_algorithm.downcast<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends>(),
        rule.negative_style_value() ? Optional<CounterStyleNegativeSign> { resolve_negative_sign(*rule.negative_style_value()) } : Optional<CounterStyleNegativeSign> {},
        rule.prefix_style_value() ? Optional<CounterStyleSymbol> { string_from_style_value(*rule.prefix_style_value()) } : Optional<CounterStyleSymbol> {},
        rule.suffix_style_value() ? Optional<CounterStyleSymbol> { string_from_style_value(*rule.suffix_style_value()) } : Optional<CounterStyleSymbol> {},
        rule.range_style_value() ? Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> { resolve_range(*rule.range_style_value(), computation_context) } : Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> {},
        rule.fallback_style_value() ? Optional<FlyString> { string_from_style_value(*rule.fallback_style_value()) } : Optional<FlyString> {},
        rule.pad_style_value() ? Optional<CounterStylePad> { resolve_pad(*rule.pad_style_value(), computation_context) } : Optional<CounterStylePad> {});
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-system
Variant<Empty, CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> CounterStyleDefinition::resolve_algorithm(NonnullRefPtr<StyleValue const> const& system_style_value, RefPtr<StyleValue const> const& symbols_style_value, RefPtr<StyleValue const> const& additive_symbols_style_value, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-symbols
    // The @counter-style rule must have a valid symbols descriptor if the counter system is cyclic,
    // numeric, alphabetic, symbolic, or fixed, or a valid additive-symbols descriptor if the counter system
    // is additive; otherwise, the @counter-style does not define a counter style (but is still a valid
    // at-rule).
    return system_style_value->as_counter_style_system().value().visit(
        [&](CounterStyleSystem const& system) -> Variant<Empty, CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> {
            switch (system) {
            case CounterStyleSystem::Cyclic:
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Symbolic:
            case CounterStyleSystem::Numeric: {
                if (!symbols_style_value)
                    return {};

                auto parsed_symbols = resolve_symbols(*symbols_style_value);

                if (!system_style_value->as_counter_style_system().is_valid_symbol_count(parsed_symbols.size()))
                    return {};

                return {
                    GenericCounterStyleAlgorithm {
                        .type = system,
                        .symbol_list = move(parsed_symbols),
                    }
                };
            }
            case CounterStyleSystem::Additive: {
                if (!additive_symbols_style_value)
                    return {};

                auto additive_tuples = resolve_additive_symbols(*additive_symbols_style_value, computation_context);

                if (!system_style_value->as_counter_style_system().is_valid_additive_symbol_count(additive_tuples.size()))
                    return {};

                return { AdditiveCounterStyleAlgorithm { .symbol_list = move(additive_tuples) } };
            }
            }

            VERIFY_NOT_REACHED();
        },
        [&](CounterStyleSystemStyleValue::Fixed const& fixed) -> Variant<Empty, CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> {
            if (!symbols_style_value)
                return {};

            auto parsed_symbols = resolve_symbols(*symbols_style_value);

            if (!system_style_value->as_counter_style_system().is_valid_symbol_count(parsed_symbols.size()))
                return {};

            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            // If it is omitted, the first symbol value is 1.
            i64 first_symbol = 1;

            if (fixed.first_symbol)
                first_symbol = int_from_style_value(fixed.first_symbol->absolutized(computation_context));

            return {
                FixedCounterStyleAlgorithm {
                    .first_symbol = first_symbol,
                    .symbol_list = move(parsed_symbols),
                }
            };
        },
        [&](CounterStyleSystemStyleValue::Extends const& extends) -> Variant<Empty, CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> {
            return extends;
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#descdef-counter-style-symbols
Vector<CounterStyleSymbol> CounterStyleDefinition::resolve_symbols(NonnullRefPtr<StyleValue const> const& symbols_style_value)
{
    auto const& entries = symbols_style_value->as_value_list().values();

    Vector<CounterStyleSymbol> symbols;
    symbols.ensure_capacity(entries.size());

    for (auto const& entry : entries)
        symbols.unchecked_append(string_from_style_value(entry));

    return symbols;
}

// https://drafts.csswg.org/css-counter-styles-3/#descdef-counter-style-additive-symbols
Vector<AdditiveCounterStyleAlgorithm::AdditiveTuple> CounterStyleDefinition::resolve_additive_symbols(NonnullRefPtr<StyleValue const> const& additive_symbols_style_value, ComputationContext const& computation_context)
{
    auto const& entries = additive_symbols_style_value->as_value_list().values();

    Vector<AdditiveCounterStyleAlgorithm::AdditiveTuple> additive_tuples;
    additive_tuples.ensure_capacity(entries.size());

    for (auto const& entry : entries) {
        auto const& tuple = entry->as_value_list().values();
        VERIFY(tuple.size() == 2);

        auto weight = AK::clamp_to<i32>(int_from_style_value(tuple[0]->absolutized(computation_context)));
        auto symbol = string_from_style_value(tuple[1]);

        additive_tuples.unchecked_append({ weight, symbol });
    }

    return additive_tuples;
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-negative
CounterStyleNegativeSign CounterStyleDefinition::resolve_negative_sign(NonnullRefPtr<StyleValue const> const& style_value)
{
    auto const& negative_entries = style_value->as_value_list().values();

    return {
        .prefix = string_from_style_value(negative_entries[0]),
        .suffix = negative_entries.size() > 1 ? string_from_style_value(negative_entries[1]) : ""_fly_string,
    };
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-range
Variant<AutoRange, Vector<CounterStyleRangeEntry>> CounterStyleDefinition::resolve_range(NonnullRefPtr<StyleValue const> const& style_value, ComputationContext const& computation_context)
{
    // auto
    // NB: Resolving auto depends on the algorithm, which we may not know at parse time i.e. if the system is 'extends'
    //     To handle this we return an intermediate value which we resolve when creating the CounterStyle.
    if (style_value->has_auto())
        return AutoRange {};

    // [ [ <integer> | infinite ]{2} ]#
    // This defines a comma-separated list of ranges. For each individual range, the first value is the lower bound and
    // the second value is the upper bound. This range is inclusive - it contains both the lower and upper bound
    // numbers. If infinite is used as the first value in a range, it represents negative infinity; if used as the
    // second value, it represents positive infinity. The range of the counter style is the union of all the ranges
    // defined in the list.
    auto const& range_entries = style_value->as_value_list().values();

    Vector<CounterStyleRangeEntry> ranges;
    ranges.ensure_capacity(range_entries.size());

    for (auto const& entry : range_entries) {
        auto const& range_values = entry->as_value_list().values();
        VERIFY(range_values.size() == 2);

        auto const resolve_value = [&](NonnullRefPtr<StyleValue const> const& value, i64 infinite_value) {
            if (value->is_keyword() && value->to_keyword() == Keyword::Infinite)
                return infinite_value;

            return int_from_style_value(value->absolutized(computation_context));
        };

        ranges.unchecked_append({ resolve_value(range_values[0], NumericLimits<i64>::min()), resolve_value(range_values[1], NumericLimits<i64>::max()) });
    }

    return ranges;
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-pad
CounterStylePad CounterStyleDefinition::resolve_pad(NonnullRefPtr<StyleValue const> const& style_value, ComputationContext const& computation_context)
{
    auto const& pad_entries = style_value->as_value_list().values();

    return CounterStylePad {
        .minimum_length = AK::clamp_to<i32>(int_from_style_value(pad_entries[0]->absolutized(computation_context))),
        .symbol = string_from_style_value(pad_entries[1]),
    };
}

}
