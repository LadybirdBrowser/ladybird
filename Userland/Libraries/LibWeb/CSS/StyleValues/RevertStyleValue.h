/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class RevertStyleValue final : public StyleValueWithDefaultOperators<RevertStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RevertStyleValue> the()
    {
        static ValueComparingNonnullRefPtr<RevertStyleValue> instance = adopt_ref(*new (nothrow) RevertStyleValue);
        return instance;
    }
    virtual ~RevertStyleValue() override = default;

    String to_string() const override { return "revert"_string; }

    bool properties_equal(RevertStyleValue const&) const { return true; }

private:
    RevertStyleValue()
        : StyleValueWithDefaultOperators(Type::Revert)
    {
    }
};

}
