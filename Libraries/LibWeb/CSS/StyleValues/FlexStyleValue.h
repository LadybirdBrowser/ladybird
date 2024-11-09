/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class FlexStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<FlexStyleValue> create(Flex flex)
    {
        return adopt_ref(*new (nothrow) FlexStyleValue(move(flex)));
    }
    virtual ~FlexStyleValue() override = default;

    Flex const& flex() const { return m_flex; }
    virtual double value() const override { return m_flex.raw_value(); }
    virtual StringView unit() const override { return m_flex.unit_name(); }

    virtual String to_string() const override { return m_flex.to_string(); }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_flex = other.as_flex();
        return m_flex == other_flex.m_flex;
    }

private:
    FlexStyleValue(Flex&& flex)
        : CSSUnitValue(Type::Flex)
        , m_flex(flex)
    {
    }

    Flex m_flex;
};

}
