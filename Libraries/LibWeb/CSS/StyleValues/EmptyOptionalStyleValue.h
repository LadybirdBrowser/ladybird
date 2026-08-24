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

    // NB: This style is used to represent a missing optional value, it should only appear within a StyleValueList which
    //     will skip serializing it and the relevant separator so it should never be serialized.
    void serialize(StringBuilder&, SerializationMode) const { VERIFY_NOT_REACHED(); }

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
