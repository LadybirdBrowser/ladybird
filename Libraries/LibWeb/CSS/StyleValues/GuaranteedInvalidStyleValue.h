/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
class GuaranteedInvalidStyleValue final : public StyleValueWithDefaultOperators<GuaranteedInvalidStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GuaranteedInvalidStyleValue> create()
    {
        static ValueComparingNonnullRefPtr<GuaranteedInvalidStyleValue> instance = adopt_ref(*new (nothrow) GuaranteedInvalidStyleValue());
        return instance;
    }
    virtual ~GuaranteedInvalidStyleValue() override = default;
    virtual String to_string(SerializationMode) const override { return {}; }

    bool properties_equal(GuaranteedInvalidStyleValue const&) const { return true; }

private:
    GuaranteedInvalidStyleValue()
        : StyleValueWithDefaultOperators(Type::GuaranteedInvalid)
    {
    }
};

}
