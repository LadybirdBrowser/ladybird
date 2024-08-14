/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class UnsetStyleValue final : public StyleValueWithDefaultOperators<UnsetStyleValue> {
public:
    static ValueComparingNonnullRefPtr<UnsetStyleValue> the()
    {
        static ValueComparingNonnullRefPtr<UnsetStyleValue> instance = adopt_ref(*new (nothrow) UnsetStyleValue);
        return instance;
    }
    virtual ~UnsetStyleValue() override = default;

    String to_string() const override { return "unset"_string; }

    bool properties_equal(UnsetStyleValue const&) const { return true; }

private:
    UnsetStyleValue()
        : StyleValueWithDefaultOperators(Type::Unset)
    {
    }
};

}
