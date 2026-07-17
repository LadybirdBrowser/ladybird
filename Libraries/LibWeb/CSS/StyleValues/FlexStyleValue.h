/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>

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

    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override { flex().serialize(builder, mode); }

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_flex = other.as_flex();
        return flex() == other_flex.flex();
    }

    virtual bool is_computationally_independent() const override { return true; }

private:
    FlexStyleValue(Flex&& flex)
        : DimensionStyleValue(Type::Flex)
        , m_value(StyleValueFFI::rust_style_value_create_flex(flex.raw_value(), to_underlying(flex.unit())))
    {
    }

    RustStyleValueHandle m_value;
};

}
