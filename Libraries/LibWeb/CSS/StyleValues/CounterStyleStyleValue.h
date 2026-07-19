/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CounterStyleStyleValue : public StyleValueWithDefaultOperators<CounterStyleStyleValue> {

public:
    struct SymbolsFunction {
        SymbolsType type;
        Vector<Utf16FlyString> symbols;

        bool operator==(SymbolsFunction const& other) const = default;
    };

    static ValueComparingNonnullRefPtr<CounterStyleStyleValue const> create(Variant<Utf16FlyString, SymbolsFunction> value)
    {
        return adopt_ref(*new (nothrow) CounterStyleStyleValue(move(value)));
    }

    virtual ~CounterStyleStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;

    RefPtr<CounterStyle const> resolve_counter_style(StyleScope const&) const;
    Variant<Utf16FlyString, SymbolsFunction> value() const
    {
        auto const& data = m_value->counter_style;
        if (!data.is_symbols)
            return Utf16FlyString::from_raw(data.name.raw);
        Vector<Utf16FlyString> symbols;
        symbols.ensure_capacity(data.symbols.length);
        for (size_t i = 0; i < data.symbols.length; ++i)
            symbols.unchecked_append(Utf16FlyString::from_raw(data.symbols.pointer[i].raw));
        return SymbolsFunction { static_cast<SymbolsType>(data.symbols_type), move(symbols) };
    }

    bool properties_equal(CounterStyleStyleValue const& other) const { return value() == other.value(); }

private:
    explicit CounterStyleStyleValue(Variant<Utf16FlyString, SymbolsFunction> value)
        : StyleValueWithDefaultOperators(Type::CounterStyle, make_counter_style_data(value))
    {
    }

    static StyleValueFFI::StyleValueData* make_counter_style_data(Variant<Utf16FlyString, SymbolsFunction> const& value)
    {
        // The Rust allocation takes ownership of one leaked reference to each retained string.
        return value.visit(
            [](Utf16FlyString const& name) {
                return StyleValueFFI::rust_style_value_create_counter_style(false, name.to_raw_leaked(), 0, nullptr, 0);
            },
            [](SymbolsFunction const& symbols_function) {
                Vector<size_t> raws;
                raws.ensure_capacity(symbols_function.symbols.size());
                for (auto const& symbol : symbols_function.symbols)
                    raws.unchecked_append(symbol.to_raw_leaked());
                return StyleValueFFI::rust_style_value_create_counter_style(true, 0, to_underlying(symbols_function.type), raws.data(), raws.size());
            });
    }
};

}
