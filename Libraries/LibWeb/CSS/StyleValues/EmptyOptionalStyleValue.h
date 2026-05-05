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
        auto static const instance = adopt_ref(*new (nothrow) EmptyOptionalStyleValue());
        return instance;
    }

    virtual ~EmptyOptionalStyleValue() override = default;

    // NB: This style is used to represent a missing optional value, it should only appear within a StyleValueList which
    //     will skip serializing/tokenizing it and the relevant separator so it should never be serialized/tokenized.
    virtual void serialize(StringBuilder&, SerializationMode) const override { VERIFY_NOT_REACHED(); }
    virtual Vector<Parser::ComponentValue> tokenize() const override { VERIFY_NOT_REACHED(); }

    bool properties_equal(EmptyOptionalStyleValue const&) const { return true; }

    virtual bool is_computationally_independent() const override { return true; }

private:
    EmptyOptionalStyleValue()
        : StyleValueWithDefaultOperators(Type::EmptyOptional)
    {
    }
};

}
