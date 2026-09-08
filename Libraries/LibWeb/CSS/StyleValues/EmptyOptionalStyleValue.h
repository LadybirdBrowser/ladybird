/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class EmptyOptionalStyleValue final : public StyleValueWithDefaultOperators<EmptyOptionalStyleValue> {
public:
    static ValueComparingNonnullRefPtr<EmptyOptionalStyleValue> create()
    {
        static auto& instance = adopt_ref(*new (nothrow) EmptyOptionalStyleValue()).leak_ref();
        return instance;
    }

    virtual ~EmptyOptionalStyleValue() override = default;

private:
    friend class StyleValue;

    explicit EmptyOptionalStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::EmptyOptional, data)
    {
    }

    EmptyOptionalStyleValue()
        : StyleValueWithDefaultOperators(Type::EmptyOptional, StyleValueFFI::rust_style_value_create_empty_optional())
    {
    }
};

}
