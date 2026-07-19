/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CounterStyleSystemStyleValue : public StyleValueWithDefaultOperators<CounterStyleSystemStyleValue> {
public:
    NonnullRefPtr<StyleValue const> static create(CounterStyleSystem system)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(system));
    }

    NonnullRefPtr<StyleValue const> static create_fixed(RefPtr<StyleValue const> first_symbol)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(Fixed { move(first_symbol) }));
    }

    NonnullRefPtr<StyleValue const> static create_extends(Utf16FlyString name)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(Extends { move(name) }));
    }

    virtual ~CounterStyleSystemStyleValue() override = default;

    void serialize(StringBuilder& builder, SerializationMode mode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& context) const;
    bool algorithm_differs_from(CounterStyleSystemStyleValue const& other) const;
    bool is_valid_symbol_count(size_t count) const;
    bool is_valid_additive_symbol_count(size_t count) const;

    struct Fixed {
        ValueComparingRefPtr<StyleValue const> first_symbol;
        bool operator==(Fixed const&) const = default;
    };

    struct Extends {
        Utf16FlyString name;
        bool operator==(Extends const&) const = default;
    };

    using Value = Variant<CounterStyleSystem, Fixed, Extends>;
    Value value() const
    {
        auto const& data = m_value->counter_style_system;
        switch (data.kind) {
        case 0:
            return static_cast<CounterStyleSystem>(data.system);
        case 1:
            return Fixed { static_cast<StyleValue const*>(data.first_symbol.pointer) };
        default:
            return Extends { Utf16FlyString::from_raw(data.name.raw) };
        }
    }

    bool properties_equal(CounterStyleSystemStyleValue const& other) const { return value() == other.value(); }

    // NB: We only use this style value within the @counter-style at-rule so will never call this
private:
    explicit CounterStyleSystemStyleValue(Variant<CounterStyleSystem, Fixed, Extends> value)
        : StyleValueWithDefaultOperators(Type::CounterStyleSystem, make_counter_style_system_data(value))
    {
    }

    static StyleValueFFI::StyleValueData* make_counter_style_system_data(Value const& value)
    {
        // The Rust allocation takes ownership of one strong reference to the first symbol and
        // one leaked reference to the name when they are present.
        return value.visit(
            [](CounterStyleSystem system) {
                return StyleValueFFI::rust_style_value_create_counter_style_system(0, to_underlying(system), nullptr, 0);
            },
            [](Fixed const& fixed) {
                if (fixed.first_symbol)
                    fixed.first_symbol->ref();
                return StyleValueFFI::rust_style_value_create_counter_style_system(1, 0, fixed.first_symbol.ptr(), 0);
            },
            [](Extends const& extends) {
                return StyleValueFFI::rust_style_value_create_counter_style_system(2, 0, nullptr, extends.name.to_raw_leaked());
            });
    }
};

}
