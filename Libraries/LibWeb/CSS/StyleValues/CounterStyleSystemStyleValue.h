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
    virtual ~CounterStyleSystemStyleValue() override = default;
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
            return Fixed { wrap_rust_child_or_null(data.first_symbol) };
        default:
            return Extends { css_string_from_rust(&data.name) };
        }
    }

private:
    friend class StyleValue;

    explicit CounterStyleSystemStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::CounterStyleSystem, data)
    {
    }
};

}
