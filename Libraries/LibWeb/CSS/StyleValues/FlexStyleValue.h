/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>

namespace Web::CSS {

class FlexStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<FlexStyleValue const> create(Flex flex)
    {
        return adopt_ref(*new (nothrow) FlexStyleValue(move(flex)));
    }
    virtual ~FlexStyleValue() override = default;

    Flex flex() const { return Flex(m_value->flex.value, static_cast<FlexUnit>(m_value->flex.unit)); }
    virtual double raw_value() const override { return m_value->flex.value; }
    virtual Utf16FlyString unit_name() const override { return flex().unit_name(); }

    void serialize(StringBuilder& builder, SerializationMode mode) const { flex().serialize(builder, mode); }

    bool equals(StyleValue const& other) const
    {
        if (type() != other.type())
            return false;
        auto const& other_flex = other.as_flex();
        return flex() == other_flex.flex();
    }

private:
    FlexStyleValue(Flex&& flex)
        : DimensionStyleValue(Type::Flex, StyleValueFFI::rust_style_value_create_flex(flex.raw_value(), to_underlying(flex.unit())))
    {
    }
};

}
