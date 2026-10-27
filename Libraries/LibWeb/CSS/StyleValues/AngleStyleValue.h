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

namespace Web::CSS {

class AngleStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<AngleStyleValue const> create(double value, AngleUnit unit)
    {
        return adopt_ref(*new (nothrow) AngleStyleValue(value, unit));
    }
    virtual ~AngleStyleValue() override;

    AngleUnit unit() const { return static_cast<AngleUnit>(m_value->angle.unit); }
    virtual double raw_value() const override { return m_value->angle.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit AngleStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Angle, data)
    {
    }

    AngleStyleValue(double value, AngleUnit unit);
};

}
