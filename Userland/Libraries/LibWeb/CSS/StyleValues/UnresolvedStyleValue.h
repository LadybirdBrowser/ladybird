/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

class UnresolvedStyleValue final : public CSSStyleValue {
public:
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue> create(Vector<Parser::ComponentValue>&& values, bool contains_var_or_attr)
    {
        return adopt_ref(*new (nothrow) UnresolvedStyleValue(move(values), contains_var_or_attr));
    }
    virtual ~UnresolvedStyleValue() override = default;

    virtual String to_string() const override;

    Vector<Parser::ComponentValue> const& values() const { return m_values; }
    bool contains_var_or_attr() const { return m_contains_var_or_attr; }

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    UnresolvedStyleValue(Vector<Parser::ComponentValue>&& values, bool contains_var_or_attr)
        : CSSStyleValue(Type::Unresolved)
        , m_values(move(values))
        , m_contains_var_or_attr(contains_var_or_attr)
    {
    }

    Vector<Parser::ComponentValue> m_values;
    bool m_contains_var_or_attr { false };
};

}
