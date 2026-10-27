/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

class FlexStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<FlexStyleValue const> create(double value, FlexUnit unit)
    {
        return adopt_ref(*new (nothrow) FlexStyleValue(value, unit));
    }
    virtual ~FlexStyleValue() override = default;

    FlexUnit unit() const { return static_cast<FlexUnit>(m_value->flex.unit); }
    virtual double raw_value() const override { return m_value->flex.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit FlexStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Flex, data)
    {
    }

    FlexStyleValue(double value, FlexUnit unit)
        : DimensionStyleValue(Type::Flex, StyleValueFFI::rust_style_value_create_flex(value, to_underlying(unit)))
    {
    }
};

}
