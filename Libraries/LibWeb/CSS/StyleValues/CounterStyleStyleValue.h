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
    static ValueComparingNonnullRefPtr<CounterStyleStyleValue const> create(Utf16FlyString const& name)
    {
        // The Rust allocation takes ownership of one leaked reference to the name.
        return adopt_ref(*new (nothrow) CounterStyleStyleValue(StyleValueFFI::rust_style_value_create_counter_style(name.to_raw_leaked())));
    }

    virtual ~CounterStyleStyleValue() override = default;

private:
    friend class StyleValue;

    explicit CounterStyleStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::CounterStyle, data)
    {
    }
};

}
