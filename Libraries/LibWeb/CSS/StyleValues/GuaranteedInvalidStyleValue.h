/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
class GuaranteedInvalidStyleValue final : public StyleValueWithDefaultOperators<GuaranteedInvalidStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GuaranteedInvalidStyleValue> create()
    {
        static auto& instance = adopt_ref(*new (nothrow) GuaranteedInvalidStyleValue()).leak_ref();
        return instance;
    }
    virtual ~GuaranteedInvalidStyleValue() override = default;
    void serialize(StringBuilder&, SerializationMode) const { }
    Vector<Parser::ComponentValue> tokenize() const
    {
        return { Parser::ComponentValue { Parser::GuaranteedInvalidValue {} } };
    }

    bool properties_equal(GuaranteedInvalidStyleValue const&) const { return true; }

private:
    GuaranteedInvalidStyleValue()
        : StyleValueWithDefaultOperators(Type::GuaranteedInvalid, StyleValueFFI::rust_style_value_create_guaranteed_invalid())
    {
    }
};

}
