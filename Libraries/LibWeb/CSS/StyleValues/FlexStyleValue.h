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
    virtual ~FlexStyleValue() override = default;

    Flex flex() const { return Flex(m_value->flex.value, static_cast<FlexUnit>(m_value->flex.unit)); }
    virtual double raw_value() const override { return m_value->flex.value; }
    virtual Utf16FlyString unit_name() const override { return flex().unit_name(); }

private:
    friend class StyleValue;

    explicit FlexStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Flex, data)
    {
    }
};

}
