/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>

namespace Web::CSS {

class PercentageStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<PercentageStyleValue const> create(Percentage percentage)
    {
        // The 0%, 50% and 100% values dominate real-world percentages, so they are interned.
        if (percentage.value() == 0) {
            static auto const& zero_instance = adopt_ref(*new (nothrow) PercentageStyleValue(Percentage(0))).leak_ref();
            return zero_instance;
        }
        if (percentage.value() == 50) {
            static auto const& fifty_instance = adopt_ref(*new (nothrow) PercentageStyleValue(Percentage(50))).leak_ref();
            return fifty_instance;
        }
        if (percentage.value() == 100) {
            static auto const& hundred_instance = adopt_ref(*new (nothrow) PercentageStyleValue(Percentage(100))).leak_ref();
            return hundred_instance;
        }
        return adopt_ref(*new (nothrow) PercentageStyleValue(move(percentage)));
    }
    virtual ~PercentageStyleValue() override = default;

    Percentage percentage() const { return Percentage(m_value->percentage.value); }
    virtual double raw_value() const override { return m_value->percentage.value; }
    virtual Utf16FlyString unit_name() const override { return "percent"_utf16_fly_string; }

    void serialize(StringBuilder& builder, SerializationMode) const { builder.append(percentage().to_string()); }

    bool equals(StyleValue const& other) const
    {
        if (type() != other.type())
            return false;
        auto const& other_percentage = other.as_percentage();
        return percentage() == other_percentage.percentage();
    }

private:
    PercentageStyleValue(Percentage&& percentage)
        : DimensionStyleValue(Type::Percentage, StyleValueFFI::rust_style_value_create_percentage(percentage.value()))
    {
    }
};

}
