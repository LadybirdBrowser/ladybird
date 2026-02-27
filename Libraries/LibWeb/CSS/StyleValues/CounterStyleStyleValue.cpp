/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleStyleValue.h"
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void CounterStyleStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    m_value.visit(
        [&](FlyString const& name) {
            builder.append(name);
        },
        [&](SymbolsFunction const& symbols_function) {
            builder.append("symbols("sv);
            if (symbols_function.type != SymbolsType::Symbolic)
                builder.appendff("{} ", CSS::to_string(symbols_function.type));

            for (size_t i = 0; i < symbols_function.symbols.size(); ++i) {
                if (i > 0)
                    builder.append(' ');
                serialize_a_string(builder, symbols_function.symbols[i]);
            }
            builder.append(')');
        });
}

RefPtr<CounterStyle const> CounterStyleStyleValue::resolve_counter_style(HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const& registered_counter_styles) const
{
    return m_value.visit(
        [&](FlyString const& name) -> RefPtr<CounterStyle const> {
            return registered_counter_styles.get(name).value_or(nullptr);
        },
        [&](SymbolsFunction const& symbols_function) -> RefPtr<CounterStyle const> {
            // https://drafts.csswg.org/css-counter-styles-3/#symbols-function

            auto algorithm = [&]() -> CounterStyleAlgorithm {
                // The counter style’s algorithm is constructed by consulting the previous chapter using the provided
                // system — or symbolic if the system was omitted — and the provided <string>s and <image>s as the value
                // of the symbols property. If the system is fixed, the first symbol value is 1.
                switch (symbols_function.type) {
                case SymbolsType::Cyclic:
                    return GenericCounterStyleAlgorithm { CounterStyleSystem::Cyclic, symbols_function.symbols };
                case SymbolsType::Numeric:
                    return GenericCounterStyleAlgorithm { CounterStyleSystem::Numeric, symbols_function.symbols };
                case SymbolsType::Alphabetic:
                    return GenericCounterStyleAlgorithm { CounterStyleSystem::Alphabetic, symbols_function.symbols };
                case SymbolsType::Symbolic:
                    return GenericCounterStyleAlgorithm { CounterStyleSystem::Symbolic, symbols_function.symbols };
                case SymbolsType::Fixed:
                    return FixedCounterStyleAlgorithm { .first_symbol = 1, .symbol_list = symbols_function.symbols };
                }

                VERIFY_NOT_REACHED();
            }();

            // The symbols() function defines an anonymous counter style with no name, a prefix of "" (empty string) and
            // suffix of " " (U+0020 SPACE), a range of auto, a fallback of decimal, a negative of "\2D" ("-"
            // hyphen-minus), a pad of 0 "", and a speak-as of auto.
            // FIXME: Pass the correct speak-as value once we support that.

            auto definition = CounterStyleDefinition::create(
                // NB: We use empty string instead of no name for simplicity - this doesn't clash with
                //     <counter-style-name> since that is a <custom-ident> which can't be an empty string.
                ""_fly_string,
                algorithm,
                CounterStyleNegativeSign { "-"_fly_string, ""_fly_string },
                ""_fly_string,
                " "_fly_string,
                AutoRange {},
                "decimal"_fly_string,
                CounterStylePad { 0, ""_fly_string });

            // NB: We don't need to pass registered counter styles here since we don't rely on extension.
            return CounterStyle::from_counter_style_definition(definition, {});
        });
}

}
