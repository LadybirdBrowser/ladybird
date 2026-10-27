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

namespace Web::CSS {

class PercentageStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<PercentageStyleValue const> create(double value)
    {
        // The 0%, 50% and 100% values dominate real-world percentages, so they are interned.
        if (value == 0) {
            static auto const& zero_instance = adopt_ref(*new (nothrow) PercentageStyleValue(0.0)).leak_ref();
            return zero_instance;
        }
        if (value == 50) {
            static auto const& fifty_instance = adopt_ref(*new (nothrow) PercentageStyleValue(50.0)).leak_ref();
            return fifty_instance;
        }
        if (value == 100) {
            static auto const& hundred_instance = adopt_ref(*new (nothrow) PercentageStyleValue(100.0)).leak_ref();
            return hundred_instance;
        }
        return adopt_ref(*new (nothrow) PercentageStyleValue(value));
    }
    virtual ~PercentageStyleValue() override = default;

    virtual double raw_value() const override { return m_value->percentage.value; }
    virtual Utf16FlyString unit_name() const override { return "percent"_utf16_fly_string; }

private:
    friend class StyleValue;

    explicit PercentageStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Percentage, data)
    {
    }

    explicit PercentageStyleValue(double value)
        : DimensionStyleValue(Type::Percentage, StyleValueFFI::rust_style_value_create_percentage(value))
    {
    }
};

}
