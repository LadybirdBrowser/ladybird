/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class InitialStyleValue final : public StyleValueWithDefaultOperators<InitialStyleValue> {
public:
    static ValueComparingNonnullRefPtr<InitialStyleValue> the()
    {
        static ValueComparingNonnullRefPtr<InitialStyleValue> instance = adopt_ref(*new (nothrow) InitialStyleValue);
        return instance;
    }
    virtual ~InitialStyleValue() override = default;

    String to_string() const override { return "initial"_string; }

    bool properties_equal(InitialStyleValue const&) const { return true; }

private:
    InitialStyleValue()
        : StyleValueWithDefaultOperators(Type::Initial)
    {
    }
};

}
