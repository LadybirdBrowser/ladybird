/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Frequency.h>

namespace Web::CSS {

class FrequencyStyleValue : public StyleValueWithDefaultOperators<FrequencyStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FrequencyStyleValue> create(Frequency frequency)
    {
        return adopt_ref(*new (nothrow) FrequencyStyleValue(move(frequency)));
    }
    virtual ~FrequencyStyleValue() override = default;

    Frequency const& frequency() const { return m_frequency; }

    virtual String to_string() const override { return m_frequency.to_string(); }

    bool properties_equal(FrequencyStyleValue const& other) const { return m_frequency == other.m_frequency; }

private:
    explicit FrequencyStyleValue(Frequency frequency)
        : StyleValueWithDefaultOperators(Type::Frequency)
        , m_frequency(move(frequency))
    {
    }

    Frequency m_frequency;
};

}
