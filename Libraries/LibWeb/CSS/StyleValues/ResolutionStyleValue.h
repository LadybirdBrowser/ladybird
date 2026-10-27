/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

class ResolutionStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<ResolutionStyleValue const> create(double value, ResolutionUnit unit)
    {
        return adopt_ref(*new (nothrow) ResolutionStyleValue(value, unit));
    }
    virtual ~ResolutionStyleValue() override = default;

    ResolutionUnit unit() const { return static_cast<ResolutionUnit>(m_value->resolution.unit); }
    virtual double raw_value() const override { return m_value->resolution.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit ResolutionStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Resolution, data)
    {
    }

    ResolutionStyleValue(double value, ResolutionUnit unit)
        : DimensionStyleValue(Type::Resolution, StyleValueFFI::rust_style_value_create_resolution(value, to_underlying(unit)))
    {
    }
};

}
