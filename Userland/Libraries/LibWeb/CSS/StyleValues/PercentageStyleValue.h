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
#include <LibWeb/CSS/Percentage.h>

namespace Web::CSS {

class PercentageStyleValue final : public StyleValueWithDefaultOperators<PercentageStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PercentageStyleValue> create(Percentage percentage)
    {
        return adopt_ref(*new (nothrow) PercentageStyleValue(move(percentage)));
    }
    virtual ~PercentageStyleValue() override = default;

    Percentage const& percentage() const { return m_percentage; }
    Percentage& percentage() { return m_percentage; }

    virtual String to_string() const override { return m_percentage.to_string(); }

    bool properties_equal(PercentageStyleValue const& other) const { return m_percentage == other.m_percentage; }

private:
    PercentageStyleValue(Percentage&& percentage)
        : StyleValueWithDefaultOperators(Type::Percentage)
        , m_percentage(percentage)
    {
    }

    Percentage m_percentage;
};

}
