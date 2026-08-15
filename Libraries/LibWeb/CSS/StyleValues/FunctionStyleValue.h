/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FunctionStyleValue : public StyleValueWithDefaultOperators<FunctionStyleValue> {
public:
    static NonnullRefPtr<FunctionStyleValue> create(Utf16FlyString name, NonnullRefPtr<StyleValue const> value)
    {
        return adopt_ref(*new FunctionStyleValue(move(name), move(value)));
    }

    Utf16FlyString name() const { return Utf16FlyString::from_raw(m_value->function.name.raw); }
    ValueComparingNonnullRefPtr<StyleValue const> value() const { return wrap_rust_child(m_value->function.value); }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit FunctionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Function, data)
    {
    }

    FunctionStyleValue(Utf16FlyString name, NonnullRefPtr<StyleValue const> value)
        : StyleValueWithDefaultOperators(Type::Function, StyleValueFFI::rust_style_value_create_function(name.to_raw_leaked(), StyleValueFFI::rust_style_value_retain(value->rust_style_value_data())))
    {
    }

    virtual ~FunctionStyleValue() override = default;
};

}
