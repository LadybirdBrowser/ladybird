/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class InheritStyleValue final : public StyleValueWithDefaultOperators<InheritStyleValue> {
public:
    static ValueComparingNonnullRefPtr<InheritStyleValue> the()
    {
        static ValueComparingNonnullRefPtr<InheritStyleValue> instance = adopt_ref(*new (nothrow) InheritStyleValue);
        return instance;
    }
    virtual ~InheritStyleValue() override = default;

    String to_string() const override { return "inherit"_string; }

    bool properties_equal(InheritStyleValue const&) const { return true; }

private:
    InheritStyleValue()
        : StyleValueWithDefaultOperators(Type::Inherit)
    {
    }
};

}
