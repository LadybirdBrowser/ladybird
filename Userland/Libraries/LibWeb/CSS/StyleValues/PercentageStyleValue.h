/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class PercentageStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<PercentageStyleValue> create(Percentage percentage)
    {
        return adopt_ref(*new (nothrow) PercentageStyleValue(move(percentage)));
    }
    virtual ~PercentageStyleValue() override = default;

    Percentage const& percentage() const { return m_percentage; }
    virtual double value() const override { return m_percentage.value(); }
    virtual StringView unit() const override { return "percent"sv; }

    virtual String to_string() const override { return m_percentage.to_string(); }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_percentage = other.as_percentage();
        return m_percentage == other_percentage.m_percentage;
    }

private:
    PercentageStyleValue(Percentage&& percentage)
        : CSSUnitValue(Type::Percentage)
        , m_percentage(percentage)
    {
    }

    Percentage m_percentage;
};

}
