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

class FrequencyStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<FrequencyStyleValue const> create(double value, FrequencyUnit unit)
    {
        return adopt_ref(*new (nothrow) FrequencyStyleValue(value, unit));
    }
    virtual ~FrequencyStyleValue() override = default;

    FrequencyUnit unit() const { return static_cast<FrequencyUnit>(m_value->frequency.unit); }
    virtual double raw_value() const override { return m_value->frequency.value; }
    virtual Utf16FlyString unit_name() const override { return CSS::to_string(unit()); }

private:
    friend class StyleValue;

    explicit FrequencyStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Frequency, data)
    {
    }

    FrequencyStyleValue(double value, FrequencyUnit unit)
        : DimensionStyleValue(Type::Frequency, StyleValueFFI::rust_style_value_create_frequency(value, to_underlying(unit)))
    {
    }
};

}
