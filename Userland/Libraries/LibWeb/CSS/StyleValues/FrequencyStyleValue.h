/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class FrequencyStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<FrequencyStyleValue> create(Frequency frequency)
    {
        return adopt_ref(*new (nothrow) FrequencyStyleValue(move(frequency)));
    }
    virtual ~FrequencyStyleValue() override = default;

    Frequency const& frequency() const { return m_frequency; }
    virtual double value() const override { return m_frequency.raw_value(); }
    virtual StringView unit() const override { return m_frequency.unit_name(); }

    virtual String to_string() const override { return m_frequency.to_string(); }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_frequency = other.as_frequency();
        return m_frequency == other_frequency.m_frequency;
    }

private:
    explicit FrequencyStyleValue(Frequency frequency)
        : CSSUnitValue(Type::Frequency)
        , m_frequency(move(frequency))
    {
    }

    Frequency m_frequency;
};

}
