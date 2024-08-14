/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Flex.h>

namespace Web::CSS {

class FlexStyleValue final : public StyleValueWithDefaultOperators<FlexStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FlexStyleValue> create(Flex flex)
    {
        return adopt_ref(*new (nothrow) FlexStyleValue(move(flex)));
    }
    virtual ~FlexStyleValue() override = default;

    Flex const& flex() const { return m_flex; }
    Flex& flex() { return m_flex; }

    virtual String to_string() const override { return m_flex.to_string(); }

    bool properties_equal(FlexStyleValue const& other) const { return m_flex == other.m_flex; }

private:
    FlexStyleValue(Flex&& flex)
        : StyleValueWithDefaultOperators(Type::Flex)
        , m_flex(flex)
    {
    }

    Flex m_flex;
};

}
