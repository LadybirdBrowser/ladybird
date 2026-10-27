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

class TimeStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<TimeStyleValue const> create(double value, TimeUnit unit)
    {
        return adopt_ref(*new (nothrow) TimeStyleValue(value, unit));
    }
    virtual ~TimeStyleValue() override = default;

    TimeUnit unit() const { return static_cast<TimeUnit>(m_value->time.unit); }
    virtual double raw_value() const override { return m_value->time.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit TimeStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Time, data)
    {
    }

    TimeStyleValue(double value, TimeUnit unit)
        : DimensionStyleValue(Type::Time, StyleValueFFI::rust_style_value_create_time(value, to_underlying(unit)))
    {
    }
};

}
