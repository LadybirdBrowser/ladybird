/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API LengthStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<LengthStyleValue const> create(double value, LengthUnit);
    static ValueComparingNonnullRefPtr<LengthStyleValue const> create_px(double value) { return create(value, LengthUnit::Px); }
    static ValueComparingNonnullRefPtr<LengthStyleValue const> create_px(CSSPixels value) { return create_px(value.to_double()); }
    virtual ~LengthStyleValue() override = default;

    LengthUnit unit() const { return static_cast<LengthUnit>(m_value->length.unit); }
    virtual double raw_value() const override { return m_value->length.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit LengthStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Length, data)
    {
    }

    LengthStyleValue(double value, LengthUnit unit)
        : DimensionStyleValue(Type::Length, StyleValueFFI::rust_style_value_create_length(value, to_underlying(unit)))
    {
    }
};

}
